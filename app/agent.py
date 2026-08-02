#!/usr/bin/env python3
# app/agent.py
"""
Единый мост Understanding <-> C-ядро NeuroCore/KOSMOS.

Zero-Hardcode Mind Loop:
  NL text
    --[LLM: understand]--> goal_id (стабильный) + params (per-call)
    --[IPC learn+think]--> WorkingMemory (C, асинхронно)
    --[C: MainLoop/CorePlanner/vm_pool]--> Episode (успех/провал)
    (если Episode не появился = алгоритма для этой природы задачи нет)
    --[LLM: synthesize]--> Pipeline-байткод (JSON)
    --[IPC learn type=pipeline + HAS_ALGORITHM + clear_cooldown]--> LMDB
    --[повторная активация]--> Episode
    --[IPC execute_op exec_algorithm ИЛИ get_property]--> сырой результат
    --[LLM: render]--> NL-ответ
"""
import hashlib
import json
import re
import sys
import time
from pathlib import Path

APP_DIR = Path(__file__).resolve().parent
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

from core.ipc import IPCClient
from core.llm import LLMClient

HYPER_VALUE_MASK = 0x3FFFFFFFFFFFFFFF


def djb2_hash(s: str) -> int:
    """Побитово совпадает с core/src/math/hash.c::djb2_hash()."""
    h = 5381
    for b in s.encode("utf-8"):
        h = ((h << 5) + h + b) & 0xFFFFFFFFFFFFFFFF
    return h & HYPER_VALUE_MASK


def _parse_json(raw: str):
    if not raw:
        return None
    raw = re.sub(r"^```(json)?", "", raw.strip()).strip()
    raw = re.sub(r"```$", "", raw).strip()
    try:
        return json.loads(raw)
    except json.JSONDecodeError:
        pass
    m = re.search(r"\{.*\}", raw, re.DOTALL)
    if m:
        try:
            return json.loads(m.group(0))
        except json.JSONDecodeError:
            return None
    return None


def _payload(resp: dict) -> dict:
    p = resp.get("payload", {})
    if isinstance(p, str):
        p = json.loads(p) if p.strip() else {}
    return p


# ---------------------------------------------------------------------
# LLM-промпты (единственное место, где живёт "понимание" — не C, не if/else)
# ---------------------------------------------------------------------

UNDERSTAND_PROMPT = """Ты — модуль Understanding когнитивного ядра NeuroCore.
Переведи запрос пользователя в СТАБИЛЬНЫЙ идентификатор задачи (Goal) и
числовые входные параметры. Одна и та же ПРИРОДА задачи (например, "среднее
трёх чисел") ВСЕГДА должна получать один и тот же goal_id — меняются только
параметры. Это критично: goal_id используется системой, чтобы находить и
переиспользовать уже изученные алгоритмы, а не синтезировать новый при
каждом обращении.

Правила:
- goal_id: snake_case, ТОЛЬКО природа задачи, без конкретных чисел из
  запроса. "среднее из 5, 10, 15" и "среднее из 100, 200, 300" -> ОДИН и тот
  же goal_id (например "average_of_three_numbers").
- params: словарь "<номер_регистра>": <целое число>. Используй регистры
  1..7 (регистр 5 зарезервирован системой — НЕ используй его). Только
  целые числа (int64). Для дробных величин укажи общий множитель scale.
- Если запрос не сводится к конкретному вычислению (открытый вопрос,
  рассуждение) — верни "kind":"reasoning" и params:{{}}.

Верни СТРОГО JSON, без пояснений:
{{
  "goal_id": "average_of_three_numbers",
  "description": "Вычислить среднее арифметическое трёх целых чисел",
  "kind": "compute",
  "params": {{"1": 5, "2": 10, "3": 15}},
  "scale": 1
}}

Запрос пользователя:
\"\"\"{text}\"\"\"
"""

SYNTHESIZE_PROMPT = """Ты — компилятор алгоритмов виртуальной машины NeuroCore.
Система искала готовый алгоритм для задачи "{description}" (goal_id={goal_id})
и не нашла его в памяти. Синтезируй НОВЫЙ алгоритм как последовательность
шагов безопасного DSL.

Доступные операции (СТРОГО только эти):
  {{"op":"load_const","dst":R,"value":N}}    R = целочисленная константа N
  {{"op":"add","dst":R,"a":R1,"b":R2}}       R = R1 + R2
  {{"op":"sub","dst":R,"a":R1,"b":R2}}       R = R1 - R2
  {{"op":"mul","dst":R,"a":R1,"b":R2}}       R = R1 * R2
  {{"op":"div","dst":R,"a":R1,"b":R2}}       R = R1 / R2  (целочисленно)
  {{"op":"move","dst":R,"src":R1}}           R = R1

Правила:
- Входные параметры УЖЕ лежат в регистрах {param_registers} — не создавай
  для них load_const, они установлены системой перед запуском.
- Регистры 8..40 — для промежуточных вычислений. 1..7 заняты входом,
  41..63 зарезервированы рантаймом — не используй их.
- Все числа целые (int64). Вход уже масштабирован scale={scale} — выведи
  результат в ТОМ ЖЕ масштабе.
- output_register — регистр с финальным результатом.

Верни СТРОГО JSON, без пояснений:
{{"steps": [...], "output_register": 0}}
"""

RENDER_PROMPT = """Ты — голос когнитивного ядра NeuroCore. Сформулируй
короткий естественный ответ пользователю на языке его вопроса. Не упоминай
регистры, VM, LMDB, байткод, Episode — пользователь не должен видеть
внутреннюю механику. Если результат отсутствует (None) — честно скажи, что
ядро выполнило задачу, но не опубликовало структурированный результат
(и предположи, что это могло быть рассуждение, а не вычисление).

Вопрос: {text}
Сырые данные, полученные от ядра: {result}
Статус выполнения: {status}
"""


# ---------------------------------------------------------------------
# Компилятор безопасного DSL -> Pipeline JSON
# (тот же формат, что понимает knowledge/pipeline_io.c::pipeline_from_json,
# уже используемый book_loader.py/bootstrap.py)
# ---------------------------------------------------------------------

class PipelineCompiler:
    """
    steps:
      load_const {dst, value:int}
      load_str   {dst, value:str}
      add/sub/mul/div {dst, a, b}
      move       {dst, src}
      prop_set   {entity_reg, key:str, value_reg}
      tool_exec  {status_dst, interpreter:str, command:str, output_dst?}
      halt       {}  (компилятор добавит сам, если отсутствует)
    """
    def __init__(self, algo_name: str):
        self.algo_name = algo_name
        self.code = []
        self.int_consts = []
        self.str_consts = []

    def _int_const(self, value) -> int:
        idx = len(self.int_consts)
        self.int_consts.append(int(value))
        return idx

    def _str_const(self, value: str) -> int:
        idx = len(self.str_consts)
        self.str_consts.append(str(value))
        return idx

    def _emit(self, op: str, arg=()):
        a = list(arg) + [0] * (6 - len(arg))
        self.code.append({"operator_id": op, "arg": a[:6]})

    def compile(self, steps: list) -> dict:
        for s in steps:
            op = s["op"]
            if op == "load_const":
                self._emit("load_const", (s["dst"], self._int_const(s["value"])))
            elif op == "load_str":
                self._emit("load_str", (s["dst"], self._str_const(s["value"])))
            elif op in ("add", "sub", "mul", "div"):
                self._emit(op, (s["dst"], s["a"], s["b"]))
            elif op == "move":
                self._emit("move", (s["dst"], s["src"]))
            elif op == "prop_set":
                key_reg = s.get("key_reg", 58)
                self._emit("load_str", (key_reg, self._str_const(s["key"])))
                self._emit("prop_set", (s["entity_reg"], key_reg, s["value_reg"]))
            elif op == "tool_exec":
                interp_reg = s.get("interp_reg", 60)
                cmd_reg = s.get("cmd_reg", 61)
                out_reg = s.get("output_dst", 62)
                self._emit("load_str", (interp_reg, self._str_const(s.get("interpreter", "/bin/sh"))))
                self._emit("load_str", (cmd_reg, self._str_const(s["command"])))
                self._emit("tool_exec", (s["status_dst"], interp_reg, cmd_reg, out_reg))
            elif op == "halt":
                self._emit("halt")
            else:
                raise ValueError(f"unsafe or unknown DSL op: {op!r}")

        if not self.code or self.code[-1]["operator_id"] != "halt":
            self._emit("halt")

        return {
            "type": "pipeline",
            "algo_name": self.algo_name,
            "code": self.code,
            "constants": {"int_consts": self.int_consts, "str_consts": self.str_consts},
        }


# ---------------------------------------------------------------------
# Оркестрация
# ---------------------------------------------------------------------

class MindAgent:
    GOAL_UTILITY = 0.9
    EPISODE_TIMEOUT_EXISTING = 4.0
    EPISODE_TIMEOUT_AFTER_SYNTH = 8.0
    POLL_INTERVAL = 0.25

    def __init__(self, llm_provider: str = "ollama", llm_model: str = None):
        self.ipc = IPCClient()
        self.ipc.connect()
        if not self.ipc.ping():
            raise RuntimeError("NeuroCore core is not responding")
        self.llm = LLMClient(provider=llm_provider, model=llm_model)

    # ---- IPC helpers ----
    def _learn(self, payload: dict) -> dict:
        resp = self.ipc.command("learn", json.dumps(payload))
        if resp.get("name") == "error":
            raise RuntimeError(f"learn failed: {resp.get('payload')} (payload={payload})")
        return resp

    def _activate_goal(self, goal_id: str, utility: float = None):
        utility = utility if utility is not None else self.GOAL_UTILITY
        self._learn({"atoms": [
            {"process": "IS_A", "kind": "relation", "args": [goal_id, "Goal"], "confidence": 1.0}
        ]})
        self._learn({"nodes": [
            {"id": goal_id, "label": goal_id, "danger": 0.1, "utility": utility}
        ]})

    def _nudge(self):
        self.ipc.command("think")

    def _get_episodes(self, subject: str, limit: int = 10) -> list:
        resp = self.ipc.request("get_episodes", {"subject": subject, "limit": limit})
        p = resp.get("payload", [])
        if isinstance(p, str):
            p = json.loads(p) if p else []
        return p

    def _wait_for_episode(self, goal_id: str, timeout: float):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            eps = self._get_episodes(goal_id)
            if eps:
                return eps[0]
            time.sleep(self.POLL_INTERVAL)
        return None

    def _clear_cooldown(self, goal_id: str):
        self.ipc.command("clear_cooldown", json.dumps({"goal": goal_id}))

    def _get_property(self, subject: str, key: str) -> dict:
        resp = self.ipc.request("get_property", {"subject": subject, "key": key})
        p = resp.get("payload", {})
        if isinstance(p, str):
            p = json.loads(p) if p else {}
        return p

    def _register_algorithm(self, algo_name: str, goal_id: str, pipeline_json: dict):
        self._learn(pipeline_json)
        self._learn({"atoms": [
            {"process": "IS_A", "kind": "relation",
             "args": ["HAS_ALGORITHM", "GoalAlgorithmRelation"], "confidence": 1.0},
            {"process": "HAS_ALGORITHM", "kind": "relation",
             "args": [algo_name, goal_id], "confidence": 1.0},
        ]})
        self._clear_cooldown(goal_id)

    def _exec_algorithm_sync(self, algo_name: str, params: dict, report_regs: list) -> dict:
        regs = {"5": algo_name}
        for k, v in params.items():
            if str(k) == "5":
                continue
            regs[str(k)] = v
        payload = {"op": "exec_algorithm", "regs": regs, "report_regs": report_regs}
        resp = self.ipc.command("execute_op", json.dumps(payload))
        return _payload(resp)

    # ---- LLM stages ----
    def understand(self, text: str) -> dict:
        raw = self.llm.query(UNDERSTAND_PROMPT.format(text=text), json_mode=True)
        data = _parse_json(raw) or {}
        data.setdefault("goal_id", "unknown_goal_" + hashlib.sha1(text.encode()).hexdigest()[:8])
        data.setdefault("params", {})
        data.setdefault("scale", 1)
        return data

    def synthesize(self, goal_id: str, description: str, params: dict, scale: int) -> dict:
        prompt = SYNTHESIZE_PROMPT.format(
            description=description, goal_id=goal_id,
            param_registers=list(params.keys()), scale=scale,
        )
        raw = self.llm.query(prompt, json_mode=True)
        dsl = _parse_json(raw)
        if not dsl or "steps" not in dsl:
            raise RuntimeError(f"LLM failed to synthesize a valid pipeline for '{goal_id}'")
        return dsl

    def render(self, text: str, result, status: str) -> str:
        prompt = RENDER_PROMPT.format(text=text, result=result, status=status)
        return self.llm.query(prompt, json_mode=False)

    # ---- Zero-Hardcode Mind Loop ----
    def think(self, text: str) -> str:
        understanding = self.understand(text)
        goal_id = understanding["goal_id"]
        description = understanding.get("description", text)
        params = understanding.get("params", {})
        scale = understanding.get("scale", 1)

        self._activate_goal(goal_id)
        self._nudge()
        episode = self._wait_for_episode(goal_id, self.EPISODE_TIMEOUT_EXISTING)

        algo_name = None
        if episode is None:
            # Алгоритма для этой природы задачи ещё нет — Understanding
            # компилирует новый Pipeline и регистрирует его как обычное
            # знание (Procedure), полностью через существующий IPC "learn".
            dsl = self.synthesize(goal_id, description, params, scale)
            algo_name = f"Synth_{goal_id}_{int(time.time())}"

            steps = list(dsl["steps"])
            out_reg = dsl.get("output_register", 0)
            if out_reg != 0:
                steps.append({"op": "move", "dst": 0, "src": out_reg})
            # Публикуем результат как свойство Goal-атома — тогда его можно
            # прочитать чисто асинхронно (get_property), без второго
            # синхронного вызова, даже спустя произвольное время.
            steps.append({"op": "load_const", "dst": 50, "value": djb2_hash(goal_id)})
            steps.append({"op": "prop_set", "entity_reg": 50, "key": "result", "value_reg": 0})

            pipeline_json = PipelineCompiler(algo_name).compile(steps)
            self._register_algorithm(algo_name, goal_id, pipeline_json)

            self._activate_goal(goal_id)
            self._nudge()
            episode = self._wait_for_episode(goal_id, self.EPISODE_TIMEOUT_AFTER_SYNTH)

        if episode is None:
            return self.render(text, None, "no_algorithm_found_or_execution_failed")

        status = "success" if episode.get("outcome", 0) >= 1.0 else "failed"

        if algo_name:
            # Мы сами только что скомпилировали этот алгоритм — знаем его
            # реальное имя, можем синхронно прочитать регистр результата
            # (djb2 — необратимая хэш-функция, обратного пути от Episode.
            # algorithm_id к имени не существует для чужих алгоритмов).
            regs = self._exec_algorithm_sync(algo_name, params, report_regs=[0])
            raw_result = regs.get("reported_regs", {}).get("0")
            result_value = (raw_result / scale) if (raw_result is not None and scale not in (0, 1)) else raw_result
        else:
            # Алгоритм нашла и выполнила система сама (существовал до этого
            # вызова) — читаем результат из property-bag Goal-атома.
            prop = self._get_property(goal_id, "result")
            result_value = prop.get("value")

        return self.render(text, result_value, status)

    def ingest(self, path_or_text, source_tag: str = None):
        from knowledge.deep_extractor import ingest_file, ingest_text
        p = Path(path_or_text)
        if p.exists():
            return ingest_file(self.ipc, self.llm, p)
        return ingest_text(self.ipc, self.llm, str(path_or_text), source_tag or "chat")


def main():
    import argparse
    ap = argparse.ArgumentParser(description="NeuroCore Mind Agent")
    ap.add_argument("--provider", default="ollama",
                     choices=["ollama", "openai", "gemini", "web_deepseek", "web_chatgpt", "web_gemini"])
    ap.add_argument("--ingest", type=Path, default=None,
                     help="Путь к файлу для Deep Ingestion (Шаг 1) вместо чата")
    args = ap.parse_args()

    agent = MindAgent(llm_provider=args.provider)

    if args.ingest:
        result = agent.ingest(args.ingest)
        print(f"[agent] ingested: {result}")
        return

    print("NeuroCore Mind Agent — Zero-Hardcode Loop. Ctrl+C для выхода.")
    while True:
        try:
            text = input("> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            break
        if not text:
            continue
        try:
            print(f"\n{agent.think(text)}\n")
        except Exception as e:
            print(f"[ERROR] {e}", file=sys.stderr)


if __name__ == "__main__":
    main()
