#!/usr/bin/env python3
# app/core/bootstrap.py
import json
import sys
import struct
from pathlib import Path

sys.path.append(str(Path(__file__).resolve().parents[1]))
from core.ipc import IPCClient
from knowledge.patterns import install_patterns
from core.sdk import djb2_hash

def is_already_bootstrapped(ipc: IPCClient) -> bool:
    resp = ipc.request("retrieve", {"query": "MetaType"})
    try:
        payload = json.loads(resp.get("payload", "{}"))
        return len(payload.get("nodes", [])) > 0 or len(payload.get("atoms", [])) > 0
    except Exception:
        return False

def float_to_uint32(f: float) -> int:
    """Конвертирует float в битовое представление uint32 для передачи в C-ядро"""
    return struct.unpack('<I', struct.pack('<f', f))[0]

def get_opcodes_map():
    """Парсит opcode.h, чтобы динамически получить правильные ID инструкций."""
    opcode_path = Path(__file__).resolve().parents[2] / "core" / "src" / "runtime" / "ops" / "opcode.h"
    op_map = {}
    if not opcode_path.exists():
        print(f"[WARN] Не найден {opcode_path}, используем хардкод.", file=sys.stderr)
        return {"OP_GLOAD_CONST": 11, "OP_ASSERT": 4} # fallback

    try:
        with open(opcode_path, "r", encoding="utf-8") as f:
            content = f.read()

        in_enum = False
        counter = 0
        for line in content.split("\n"):
            line = line.strip()
            if not line or line.startswith("//"): continue
            if "typedef enum" in line or "enum {" in line:
                in_enum = True
                continue
            if in_enum and "}" in line:
                break
            if in_enum:
                part = line.split(",")[0].strip()
                if not part: continue
                if "=" in part:
                    name, val = part.split("=")
                    name = name.strip()
                    counter = int(val.strip())
                    op_map[name] = counter
                else:
                    op_map[part] = counter
                counter += 1
    except Exception as e:
        print(f"[WARN] Ошибка парсинга opcode.h: {e}", file=sys.stderr)
        return {"OP_GLOAD_CONST": 11, "OP_ASSERT": 4} # fallback

    return op_map

def inject_core_algorithms(ipc: IPCClient):
    print("[Bootstrap] Заливаем системные алгоритмы в LMDB...")

    def learn_pipeline(name, code, consts=None):
        payload = {
            "type": "pipeline",
            "algo_name": name,
            "code": code,
            "constants": consts or {}
        }
        resp = ipc.command("learn", json.dumps(payload))
        if resp.get("name") == "error":
            print(f"[Bootstrap] Ошибка загрузки {name}: {resp.get('payload')}")
        return resp

    ops = get_opcodes_map()
    OP_GLOAD_CONST = ops.get("OP_GLOAD_CONST", 11)
    OP_ASSERT = ops.get("OP_ASSERT", 4)

    # 1. MainLoop
    learn_pipeline("MainLoop", [
        {"operator_id": "load_const", "arg": [10, 0, 0, 0, 0, 0]},
        {"operator_id": "load_const", "arg": [11, 1, 0, 0, 0, 0]},
        {"operator_id": "load_const", "arg": [12, 2, 0, 0, 0, 0]},
        {"operator_id": "load_const", "arg": [13, 3, 0, 0, 0, 0]},
        {"operator_id": "load_context", "arg": [0, 0, 0, 0, 0, 0]},
        {"operator_id": "evaluate_goals", "flags": 1, "arg": [0, 0, 0, 0, 0, 0]},
        {"operator_id": "spread_activation", "arg": [0, 0, 0, 0, 0, 0]},
        {"operator_id": "sub", "arg": [10, 10, 12, 0, 0, 0]},
        {"operator_id": "cond_branch_gt", "arg": [10, 11, 4, 0, 0, 0]},
        {"operator_id": "exec_algorithm", "arg": [13, 0, 0, 0, 0, 0]},
        {"operator_id": "halt", "arg": [0, 0, 0, 0, 0, 0]}
    ], {"int_consts": [16, 0, 1, str(djb2_hash("CriticMain"))]}) # Передаем большие хэши как строки!

    # 2. CorePlanner
    learn_pipeline("CorePlanner", [
        {"operator_id": "wm_top_goal", "arg": [1, 2, 0, 0, 0, 0]},
        {"operator_id": "branch_if_empty", "arg": [1, 5, 0, 0, 0, 0]},
        {"operator_id": "select_algorithm", "arg": [1, 0, 3, 0, 0, 0]},
        {"operator_id": "read_sp", "arg": [4, 0, 0, 0, 0, 0]},
        {"operator_id": "dispatch_async", "arg": [1, 4, 0, 0, 0, 0]},
        {"operator_id": "halt", "arg": [0, 0, 0, 0, 0, 0]}
    ])

    # 3. CriticMain
    learn_pipeline("CriticMain", [
        {"operator_id": "critic_apply", "arg": [0, 0, 0, 0, 0, 0]},
        {"operator_id": "halt", "arg": [0, 0, 0, 0, 0, 0]}
    ])

    # 4. MetaCriticGraph
    learn_pipeline("MetaCriticGraph", [
        {"operator_id": "load_const", "arg": [16, 0, 0, 0, 0, 0]},
        {"operator_id": "load_const", "arg": [18, 1, 0, 0, 0, 0]},
        {"operator_id": "cond_branch_gt", "arg": [14, 18, 6, 0, 0, 0]},
        {"operator_id": "load_fconst", "arg": [17, 0, 0, 0, 0, 0]},
        {"operator_id": "atom_reinforce", "arg": [12, 17, 0, 0, 0, 0]},
        {"operator_id": "halt", "arg": [0, 0, 0, 0, 0, 0]},
        {"operator_id": "load_fconst", "arg": [17, 1, 0, 0, 0, 0]},
        {"operator_id": "atom_reinforce", "arg": [12, 17, 0, 0, 0, 0]},
        {"operator_id": "halt", "arg": [0, 0, 0, 0, 0, 0]}
    ], {"int_consts": [0, 1], "float_consts": [0.40, -0.30]})

    # 5. InductiveExtractor
    learn_pipeline("InductiveExtractor", [
        {"operator_id": "load_const", "arg": [50, 0, 0, 0, 0, 0]},
        {"operator_id": "load_const", "arg": [46, 1, 0, 0, 0, 0]},
        {"operator_id": "load_const", "arg": [58, 2, 0, 0, 0, 0]},

        {"operator_id": "wm_top_goal", "arg": [44, 45, 0, 0, 0, 0]},
        {"operator_id": "cond_branch_gt", "arg": [45, 50, 6, 0, 0, 0]},
        {"operator_id": "halt", "arg": [0, 0, 0, 0, 0, 0]},

        {"operator_id": "mine_causal_pattern", "arg": [44, 46, 47, 48, 53, 49]},
        {"operator_id": "cond_branch_gt", "arg": [49, 50, 9, 0, 0, 0]},
        {"operator_id": "halt", "arg": [0, 0, 0, 0, 0, 0]},

        {"operator_id": "spawn_ctx", "arg": [51, 0, 0, 0, 0, 0]},
        {"operator_id": "move", "arg": [52, 50, 0, 0, 0, 0]},

        {"operator_id": "write_sp", "arg": [0, 60, 0, 0, 0, 0]},
        {"operator_id": "write_sp", "arg": [1, 0, 0, 0, 0, 0]},
        {"operator_id": "write_sp", "arg": [2, 0, 0, 0, 0, 0]},
        {"operator_id": "write_sp", "arg": [3, 0, 0, 0, 0, 0]},
        {"operator_id": "write_sp", "arg": [4, 0, 0, 0, 0, 0]},
        {"operator_id": "write_sp", "arg": [5, 0, 0, 0, 0, 0]},
        {"operator_id": "assert_instruction", "arg": [OP_GLOAD_CONST, 0, 52, 60, 44, 1]},
        {"operator_id": "move", "arg": [52, 60, 0, 0, 0, 0]},
        {"operator_id": "move", "arg": [12, 60, 0, 0, 0, 0]},

        {"operator_id": "write_sp", "arg": [0, 61, 0, 0, 0, 0]},
        {"operator_id": "write_sp", "arg": [1, 0, 0, 0, 0, 0]},
        {"operator_id": "write_sp", "arg": [2, 0, 0, 0, 0, 0]},
        {"operator_id": "write_sp", "arg": [3, 0, 0, 0, 0, 0]},
        {"operator_id": "write_sp", "arg": [4, 0, 0, 0, 0, 0]},
        {"operator_id": "write_sp", "arg": [5, 0, 0, 0, 0, 0]},
        {"operator_id": "assert_instruction", "arg": [OP_GLOAD_CONST, 0, 52, 61, 44, 1]},
        {"operator_id": "move", "arg": [52, 61, 0, 0, 0, 0]},

        {"operator_id": "write_sp", "arg": [0, 47, 0, 0, 0, 0]},
        {"operator_id": "write_sp", "arg": [1, 60, 0, 0, 0, 0]},
        {"operator_id": "write_sp", "arg": [2, 61, 0, 0, 0, 0]},
        {"operator_id": "write_sp", "arg": [3, 62, 0, 0, 0, 0]},
        {"operator_id": "write_sp", "arg": [4, 0, 0, 0, 0, 0]},
        {"operator_id": "write_sp", "arg": [5, 0, 0, 0, 0, 0]},
        {"operator_id": "assert_instruction", "arg": [OP_ASSERT, 0, 52, 62, 0, 0]},

        {"operator_id": "eval_graph", "arg": [12, 58, 14, 0, 0, 0]},
        {"operator_id": "cond_branch_gt", "arg": [14, 50, 41, 0, 0, 0]},

        {"operator_id": "load_fconst", "arg": [17, 0, 0, 0, 0, 0]},
        {"operator_id": "atom_reinforce", "arg": [62, 17, 0, 0, 0, 0]},
        {"operator_id": "merge_ctx", "arg": [float_to_uint32(0.10), 0, 0, 0, 0, 0]},
        {"operator_id": "halt", "arg": [0, 0, 0, 0, 0, 0]},

        {"operator_id": "load_fconst", "arg": [17, 1, 0, 0, 0, 0]},
        {"operator_id": "atom_reinforce", "arg": [62, 17, 0, 0, 0, 0]},
        {"operator_id": "halt", "arg": [0, 0, 0, 0, 0, 0]}
    ], {
        "int_consts": [0, 2, 16],
        "float_consts": [0.80, -0.30]
    })

    # 6. AnalogyPlanner
    learn_pipeline("AnalogyPlanner", [
        {"operator_id": "load_const", "arg": [30, 0, 0, 0, 0, 0]},
        {"operator_id": "load_const", "arg": [31, 1, 0, 0, 0, 0]},
        {"operator_id": "load_const", "arg": [41, 2, 0, 0, 0, 0]},
        {"operator_id": "wm_top_goal", "arg": [32, 33, 0, 0, 0, 0]},
        {"operator_id": "cond_branch_gt", "arg": [33, 41, 6, 0, 0, 0]},
        {"operator_id": "halt", "arg": [0, 0, 0, 0, 0, 0]},
        {"operator_id": "get_neighbors", "arg": [32, 30, 0, 34, 0, 0]},
        {"operator_id": "read_sp", "arg": [35, 0, 0, 0, 0, 0]},
        {"operator_id": "find_similar", "arg": [35, 36, 37, 0, 0, 0]},
        {"operator_id": "get_neighbors", "arg": [37, 30, 30, 34, 0, 0]},
        {"operator_id": "read_sp", "arg": [38, 30, 0, 0, 0, 0]},
        {"operator_id": "concat_paths", "arg": [50, 32, 35, 37, 38, 0]},
        {"operator_id": "derive", "arg": [30, 32, 38, 39, 40, 0]},
        {"operator_id": "halt", "arg": [0, 0, 0, 0, 0, 0]}
    ], {"int_consts": [str(djb2_hash("CAUSES")), 0, 1]}) # Передаем большие хэши как строки!

    ipc.command("learn", json.dumps({"atoms": [
        {"process": "IS_A", "kind": "relation", "args": ["InductiveSynthesisGoal", "Goal"], "confidence": 1.0},
        {"process": "HAS_ALGORITHM", "kind": "relation", "args": ["InductiveExtractor", "InductiveSynthesisGoal"], "confidence": 1.0}
    ]}))


def bootstrap_knowledge(ipc: IPCClient, force=False):
    if not force and is_already_bootstrapped(ipc):
        print("[Bootstrap] Knowledge already loaded, skipping.")
        return

    print("[Bootstrap] Loading Meta-Core...")
    meta_atoms = [
        {"process": "IS_A", "kind": "relation", "args": ["Goal", "MetaType"], "confidence": 1.0},
        {"process": "IS_A", "kind": "relation", "args": ["Algorithm", "MetaType"], "confidence": 1.0},
        {"process": "IS_A", "kind": "relation", "args": ["Relation", "MetaType"], "confidence": 1.0},
        {"process": "SOLVES", "kind": "relation", "args": ["Algorithm", "Goal"], "confidence": 1.0},
        {"process": "IS_A", "kind": "relation", "args": ["HAS_ALGORITHM", "GoalAlgorithmRelation"], "confidence": 1.0},
    ]
    resp = ipc.command("learn", json.dumps({"atoms": meta_atoms}))

    inject_core_algorithms(ipc)

    goal_atoms = [
        {"process": "IS_A", "kind": "relation", "args": ["FindVulnerability", "Goal"], "confidence": 1.0},
        {"process": "HAS_ALGORITHM", "kind": "relation", "args": ["CheckEdgeAlgo", "FindVulnerability"], "confidence": 1.0},
    ]
    ipc.command("learn", json.dumps({"atoms": goal_atoms}))

    pipeline_payload = {
        "type": "pipeline",
        "algo_name": "CheckEdgeAlgo",
        "code": [
            {"operator_id": "load_const", "arg": [0, 0, 0, 0, 0, 0]},
            {"operator_id": "load_const", "arg": [1, 1, 0, 0, 0, 0]},
            {"operator_id": "load_const", "arg": [2, 2, 0, 0, 0, 0]},
            {"operator_id": "check_cached_edge", "arg": [3, 0, 2, 1]},
            {"operator_id": "halt"}
        ],
        "constants": {
            "str_consts": ["A", "B", "CAUSES"]
        }
    }
    ipc.command("learn", json.dumps(pipeline_payload))
    install_patterns(ipc)
    print("[Bootstrap] Complete.")
