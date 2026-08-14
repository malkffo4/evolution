#!/usr/bin/env python3
# app/knowledge/knowledge_compiler.py
"""
Semantic Knowledge-to-Bytecode Compiler.

Text plan (LLM) -> abstract capability intents -> semantic tool resolution
(embed_text + OP_FIND_SIMILAR, NO hardcoded tool names) -> graph-native
PROC_KIND_INSTRUCTION chain (idx_causal_rev) -> thin linear wrapper Pipeline
(SPAWN_CTX / EVAL_GRAPH / MERGE_CTX) -> HAS_ALGORITHM(wrapper, goal).

Depends on the perception.c patch adding `"kind":"instruction"` atom
support (see accompanying C patch). Everything else goes through the
existing, unmodified `learn` / `learn_pipeline` / `link_algorithm` IPC
surface -- no new endpoints.

Transaction model: this module never touches LMDB directly. Every mutation
is a `cmd_learn` IPC command, which the C side always executes inside the
single db_writer thread's write transaction (see storage/db/db_writer.h).
Concurrent compiler invocations are therefore serialized safely by the
kernel itself; this module holds no locks of its own beyond CoreClient's
IPC-socket mutex.
"""
from __future__ import annotations

import json
import struct
import sys
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

APP_DIR = Path(__file__).resolve().parents[1]
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

from core.sdk import CoreClient, djb2_hash, parse_json
from core.bootstrap import get_opcodes_map
from knowledge.embeddings import embed_text

OPCODES = get_opcodes_map()  # {"OP_LOAD_CONST": 2, "OP_EXEC_ALGORITHM": 21, ...}

# --------------------------------------------------------------------------
# LLM prompt: methodology-level plan, NOT payloads.
#
# Deliberately mirrors the restriction already established in
# app/knowledge/domain_prompts.py (SECURITY_EXTRACTION_PROMPT): the
# compiler operates on reusable *capability intents* ("perform network
# reconnaissance"), never on ready exploit code or step-by-step attack
# payloads. Concrete tool selection happens AFTER extraction, via
# semantic matching against whatever Capabilities/Implementations are
# already known to the graph -- if nothing matches, the compiler records
# a KnowledgeGap instead of inventing an unsafe fallback.
# --------------------------------------------------------------------------
PLAN_EXTRACTION_PROMPT = """Ты -- планировщик методологии, не генератор эксплойтов.
Разбей задачу на абстрактные шаги-намерения (capability intents).

СТРОГО ЗАПРЕЩЕНО:
- готовые payload'ы, эксплойт-код, конкретные командные строки;
- имена конкретных бинарников/инструментов (их подставит семантический
  поиск по уже известным ядру Capability, а не ты).

РАЗРЕШЕНО:
- методологический уровень: "perform_network_reconnaissance",
  "identify_running_services", "enumerate_web_paths",
  "check_input_sanitization" -- общие, переиспользуемые намерения.

Верни СТРОГО JSON:
{{
  "goal_id": "snake_case_имя_цели",
  "description": "человеко-читаемое описание цели",
  "steps": [
    {{"intent": "perform_network_reconnaissance",
      "description": "Определить открытые сервисы на цели"}}
  ]
}}

Задача:
\"\"\"{objective}\"\"\"
"""

LESSON_PATCH_PROMPT = """Ты -- модуль самокоррекции когнитивного ядра.
Исходный план для цели '{goal_id}' провалился.

Исходные шаги (JSON):
{original_steps}

Собранные свидетельства провала:
{evidence}

Задача: предложи ИСПРАВЛЕННЫЙ список шагов той же схемы
({{"intent":..., "description":...}}), решающий указанную проблему.
Обычно достаточно вставить ОДИН новый шаг перед проблемным или заменить
один шаг. Не переписывай план целиком без необходимости.

Верни СТРОГО JSON:
{{
  "lesson": "одно предложение -- чему научилась система",
  "steps": [ {{"intent": "...", "description": "..."}} ]
}}
"""


def _float_bits(f: float) -> int:
    """Побитовая упаковка IEEE-754 float в uint32 -- тот же приём, что
    bootstrap.py::float_to_uint32() использует для макроса '@float:'."""
    return struct.unpack("<I", struct.pack("<f", f))[0]


# ==========================================================================
# Campaign registry: goal_name (string) <-> compiled plan.
#
# EpisodeRecorded IPC events carry only numeric ids (goal_id, algorithm_id)
# -- the kernel has no reverse hash->string endpoint by design (Principle 4:
# "Код не содержит предметных знаний"). The orchestrator (this module and
# self_correction_worker.py) is exactly where that mapping belongs: it is
# operational metadata about *which campaigns are in flight*, not knowledge
# the kernel itself needs to reason about.
# ==========================================================================
class CampaignRegistry:
    def __init__(self, path: Path = APP_DIR / ".campaign_registry.json"):
        self.path = path
        self._data: dict[str, dict] = {}
        if self.path.exists():
            try:
                self._data = json.loads(self.path.read_text(encoding="utf-8"))
            except Exception:
                self._data = {}

    def _save(self) -> None:
        self.path.write_text(json.dumps(self._data, indent=2), encoding="utf-8")

    def register(self, goal_name: str, steps: list[dict], generation: int = 0) -> None:
        entry = self._data.setdefault(goal_name, {"generations": []})
        entry["generations"].append({"generation": generation, "steps": steps})
        self._save()

    def lookup_by_goal_id(self, goal_id: int) -> Optional[tuple[str, list[dict], int]]:
        for goal_name, entry in self._data.items():
            if djb2_hash(goal_name) & 0x3FFFFFFFFFFFFFFF == (goal_id & 0x3FFFFFFFFFFFFFFF):
                gens = entry.get("generations", [])
                if not gens:
                    return None
                latest = gens[-1]
                return goal_name, latest["steps"], latest["generation"]
        return None


# ==========================================================================
# Graph-native instruction chain builder
# ==========================================================================
class GraphProgramBuilder:
    """Строит причинно связанную (idx_causal_rev) цепочку
    PROC_KIND_INSTRUCTION атомов, которую OP_EVAL_GRAPH интерпретирует
    напрямую. Регистры внутри цепочки: R40 (scratch для загрузки id
    инструмента), больше ничего не используется -- цепочка линейна,
    без внутренних ветвлений (ветвление между гипотезами реализуется
    на уровне HAS_ALGORITHM-конкуренции, не внутри одной цепочки)."""

    REG_TOOL = 40

    def __init__(self, sandbox_context: int):
        self.sandbox_context = sandbox_context
        self.atoms: list[dict] = []
        self._prev_id: Optional[str] = None
        self._n = 0

    def _next_id(self) -> str:
        self._n += 1
        return f"instr_{uuid.uuid4().hex[:10]}_{self._n}"

    def _emit(self, opcode_enum_name: str, fields: list[int], wide=None) -> str:
        node_id = self._next_id()
        atom = {
            "id": node_id,
            "kind": "instruction",
            "opcode": OPCODES[opcode_enum_name],
            "fields": (list(fields) + [0] * 6)[:6],
            "context": self.sandbox_context,
        }
        if wide is not None:
            atom["wide"] = wide
        if self._prev_id is not None:
            atom["cause"] = self._prev_id
        self.atoms.append(atom)
        self._prev_id = node_id
        return node_id

    def add_capability_call(self, tool_algo_name: str) -> None:
        """R_TOOL = wide(tool_algo_name); EXEC_ALGORITHM(R_TOOL).
        wide принимает строку -- perception.c::resolve_arg() хэширует её
        тем же djb2_hash, каким был зарегистрирован сам algo_id."""
        self._emit("OP_GLOAD_CONST", [self.REG_TOOL, 0, 0, 0, 0, 0], wide=tool_algo_name)
        self._emit("OP_EXEC_ALGORITHM", [self.REG_TOOL, 0, 0, 0, 0, 0])

    @property
    def head_id(self) -> Optional[str]:
        return self.atoms[0]["id"] if self.atoms else None

    @property
    def head_numeric_id(self) -> int:
        return djb2_hash(self.head_id)


# ==========================================================================
# Wrapper linear Pipeline: bridges CorePlanner (which only knows
# db.graph.algorithms Pipeline blobs) to the graph-native chain.
# ==========================================================================
def build_wrapper_pipeline(head_atom_id: int, max_steps: int = 128,
                            success_threshold: float = 0.30,
                            fail_threshold: float = 0.95) -> dict:
    """
    Регистры:
      R1 = head instruction atom id
      R2 = max_steps
      R4 = 0 (порог сравнения статуса)
      R5 = id новой песочницы (информационно, из spawn_ctx)
      R3 = статус, возвращённый eval_graph (VM_OK=0)
    """
    int_consts = [head_atom_id, max_steps, 0]
    code = [
        {"operator_id": "load_const", "arg": [1, 0, 0, 0, 0, 0]},   # r1 = head_id
        {"operator_id": "load_const", "arg": [2, 1, 0, 0, 0, 0]},   # r2 = max_steps
        {"operator_id": "load_const", "arg": [4, 2, 0, 0, 0, 0]},   # r4 = 0
        {"operator_id": "spawn_ctx",  "arg": [5, 0, 0, 0, 0, 0]},   # r5 = sandbox ctx
        {"operator_id": "eval_graph", "arg": [1, 2, 3, 0, 0, 0]},   # r3 = status
        {"operator_id": "cond_branch_gt", "arg": [3, 4, 8, 0, 0, 0]},  # status>0 -> fail(idx 8)
        {"operator_id": "merge_ctx",  "arg": [_float_bits(success_threshold), 0, 0, 0, 0, 0]},
        {"operator_id": "halt",       "arg": [0, 0, 0, 0, 0, 0]},
        {"operator_id": "merge_ctx",  "arg": [_float_bits(fail_threshold), 0, 0, 0, 0, 0]},
        {"operator_id": "halt",       "arg": [0, 0, 0, 0, 0, 0]},
    ]
    return {"code": code, "constants": {"int_consts": int_consts}}


# ==========================================================================
# Semantic capability resolution -- no hardcoded tool names.
# Mirrors the exact-then-semantic pattern of knowledge/retrieval.py.
# ==========================================================================
def resolve_capability(core: CoreClient, intent_text: str) -> Optional[dict]:
    """
    Ищет Capability, семантически ближайшую к intent_text, и находит
    исполняемый алгоритм, реализующий её: HAS_ALGORITHM(algo_name, capability_label).
    Тот же relation, которым уже связаны Goal<->Algorithm везде в кодовой
    базе -- никакой новой онтологии не вводится, Capability просто
    выступает в роли "цели" для целей этой связки.
    """
    # 1. Дёшево: если LLM (или предыдущая компиляция) уже назвала intent
    #    именем существующей Capability -- прямое совпадение.
    direct = _find_algorithm_for_capability(core, intent_text)
    if direct:
        return {"capability": intent_text, "algo_name": direct}

    # 2. Семантика: ANN-поиск по эмбеддингам сущностей (SimHash 128-dim).
    vec = embed_text(intent_text)
    neighbors = core.find_similar(vec, top_k=5)
    for n in neighbors:
        label = n.get("label")
        if not label:
            continue
        algo = _find_algorithm_for_capability(core, label)
        if algo:
            return {"capability": label, "algo_name": algo}
    return None


def _find_algorithm_for_capability(core: CoreClient, capability_label: str) -> Optional[str]:
    for atom in core.retrieve(capability_label).get("atoms", []):
        if atom.get("process") != "HAS_ALGORITHM":
            continue
        args = atom.get("args", [])
        if len(args) == 2 and args[1] == capability_label:
            return args[0]
    return None


# ==========================================================================
# Top-level orchestration
# ==========================================================================
@dataclass
class CompileResult:
    goal_name: str
    wrapper_algo_name: str
    sandbox_context: int
    resolved_steps: int
    gap_intents: list[str] = field(default_factory=list)


def compile_and_emit(core: CoreClient, goal_name: str, description: str,
                      steps: list[dict], registry: Optional[CampaignRegistry] = None,
                      generation: int = 0) -> CompileResult:
    sandbox_ctx = djb2_hash(f"sandbox::{goal_name}::{uuid.uuid4().hex[:8]}")
    chain = GraphProgramBuilder(sandbox_context=sandbox_ctx)
    gap_intents: list[str] = []

    for step in steps:
        intent = step.get("intent", "")
        match = resolve_capability(core, intent)
        if not match:
            gap_intents.append(intent)
            continue
        chain.add_capability_call(match["algo_name"])

    if not chain.atoms:
        raise RuntimeError(
            f"compile_and_emit('{goal_name}'): no step resolved to a known "
            f"capability -- nothing to execute. Unresolved: {gap_intents}"
        )

    # 1. Push the graph-instruction chain.
    core.learn({"atoms": chain.atoms})

    # 2. KnowledgeGap facts for anything that couldn't be resolved --
    #    input for the curiosity/research loop (see docs/rfc/RFC-0002.md).
    if gap_intents:
        gap_atoms = [{
            "process": "HAS_KNOWLEDGE_GAP", "kind": "event",
            "args": [goal_name, intent],
            "truth": {"mean": 1.0, "confidence": 1.0},
            "attention": {"sti": 0.3, "lti": 0.2},
        } for intent in gap_intents]
        core.learn({"atoms": gap_atoms})

    # 3. Thin linear wrapper Pipeline, the only thing algorithm_load() can see.
    wrapper_name = f"Hyp_{goal_name}_{uuid.uuid4().hex[:8]}"
    pipeline = build_wrapper_pipeline(chain.head_numeric_id)
    core.learn_pipeline(wrapper_name, pipeline["code"], pipeline["constants"])

    # 4. Link as a candidate for the goal (competes under UCB1 with any
    #    prior hypothesis already linked to this goal -- see planner_ops.c).
    core.link_algorithm(wrapper_name, goal_name)

    if registry is not None:
        registry.register(goal_name, steps, generation=generation)

    return CompileResult(
        goal_name=goal_name, wrapper_algo_name=wrapper_name,
        sandbox_context=sandbox_ctx, resolved_steps=len(steps) - len(gap_intents),
        gap_intents=gap_intents,
    )


def compile_from_llm_plan(core: CoreClient, llm, objective: str,
                           registry: Optional[CampaignRegistry] = None) -> CompileResult:
    raw = llm.query(PLAN_EXTRACTION_PROMPT.format(objective=objective), json_mode=True)
    plan = parse_json(raw)
    if not plan or "steps" not in plan:
        raise RuntimeError(f"knowledge_compiler: LLM failed to produce a valid plan for: {objective!r}")

    goal_name = plan.get("goal_id") or f"Goal_{uuid.uuid4().hex[:8]}"
    return compile_and_emit(core, goal_name, plan.get("description", objective),
                             plan["steps"], registry=registry, generation=0)


def main():
    if len(sys.argv) < 2:
        print("Usage: knowledge_compiler.py \"<objective text>\"")
        sys.exit(1)

    from core.llm import LLMClient
    core = CoreClient().connect()
    llm = LLMClient()
    registry = CampaignRegistry()

    result = compile_from_llm_plan(core, llm, sys.argv[1], registry=registry)
    print(f"[knowledge_compiler] goal='{result.goal_name}' "
          f"wrapper='{result.wrapper_algo_name}' resolved={result.resolved_steps} "
          f"gaps={result.gap_intents}")
    core.activate_goal(result.goal_name, utility=0.9)
    core.close()


if __name__ == "__main__":
    main()
