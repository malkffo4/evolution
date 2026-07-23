#!/usr/bin/env python3
import json, sys
from pathlib import Path
sys.path.append(str(Path(__file__).resolve().parents[1]))
from runtime.ipc import IPCClient

def djb2_hash(s: str) -> int:
    h = 5381
    for c in s:
        h = ((h << 5) + h) + ord(c)
    return h & 0xFFFFFFFFFFFFFFFF

def bootstrap_knowledge(ipc: IPCClient):
    """Загружает Meta-Core и алгоритм CheckEdgeAlgo через IPC."""
    # 1. Мета-типы
    meta_atoms = [
        {"process": "IS_A", "args": ["Goal", "MetaType"], "confidence": 1.0},
        {"process": "IS_A", "args": ["Algorithm", "MetaType"], "confidence": 1.0},
        {"process": "IS_A", "args": ["Relation", "MetaType"], "confidence": 1.0},
        {"process": "SOLVES", "args": ["Algorithm", "Goal"], "confidence": 1.0},
        {"process": "HAS_ALGORITHM", "args": ["Goal", "Algorithm"], "confidence": 1.0},
    ]
    resp = ipc.command("learn", json.dumps({"atoms": meta_atoms}))
    print("[Bootstrap] Meta-types:", resp.get("payload", ""))

    # 2. Цель и алгоритм
    goal_id = djb2_hash("FindVulnerability")
    algo_id = djb2_hash("CheckEdgeAlgo")
    goal_atoms = [
        {"process": "IS_A", "args": ["FindVulnerability", "Goal"], "confidence": 1.0},
        {"process": "HAS_ALGORITHM", "args": ["FindVulnerability", "CheckEdgeAlgo"], "confidence": 1.0},
    ]
    resp = ipc.command("learn", json.dumps({"atoms": goal_atoms}))
    print("[Bootstrap] Goal:", resp.get("payload", ""))

    # 3. Алгоритм (пока через старый pipeline, будет заменён на гипер-операторы)
    pipeline_payload = {
        "type": "pipeline",
        "algo_id": algo_id,
        "code": [
            {"operator_id": "OP_CHECK_CACHED_EDGE", "arg": [3, 0, 2, 1]},
            {"operator_id": "OP_HALT"}
        ]
    }
    resp = ipc.command("learn", json.dumps(pipeline_payload))
    print("[Bootstrap] Algorithm:", resp.get("payload", ""))
