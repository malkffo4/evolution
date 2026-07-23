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

def is_already_bootstrapped(ipc: IPCClient) -> bool:
    resp = ipc.request("retrieve", {"query": "MetaType"})
    try:
        payload = json.loads(resp.get("payload", "{}"))
        return len(payload.get("nodes", [])) > 0 or len(payload.get("atoms", [])) > 0
    except Exception:
        return False

def bootstrap_knowledge(ipc: IPCClient, force=False):
    if not force and is_already_bootstrapped(ipc):
        print("[Bootstrap] Knowledge already loaded, skipping.")
        return

    print("[Bootstrap] Loading Meta-Core...")
    # Мета-типы
    meta_atoms = [
        {"process": "IS_A", "args": ["Goal", "MetaType"], "confidence": 1.0},
        {"process": "IS_A", "args": ["Algorithm", "MetaType"], "confidence": 1.0},
        {"process": "IS_A", "args": ["Relation", "MetaType"], "confidence": 1.0},
        {"process": "SOLVES", "args": ["Algorithm", "Goal"], "confidence": 1.0},
        {"process": "HAS_ALGORITHM", "args": ["Goal", "Algorithm"], "confidence": 1.0},
    ]
    resp = ipc.command("learn", json.dumps({"atoms": meta_atoms}))
    print(f"[Bootstrap] Meta-types: {resp}")

    # Цель
    goal_atoms = [
        {"process": "IS_A", "args": ["FindVulnerability", "Goal"], "confidence": 1.0},
        {"process": "HAS_ALGORITHM", "args": ["FindVulnerability", "CheckEdgeAlgo"], "confidence": 1.0},
    ]
    resp = ipc.command("learn", json.dumps({"atoms": goal_atoms}))
    print(f"[Bootstrap] Goal: {resp}")

    # Алгоритм
    algo_id = djb2_hash("CheckEdgeAlgo")
    pipeline_payload = {
        "type": "pipeline",
        "algo_id": algo_id,
        "code": [
            {"operator_id": "OP_CHECK_CACHED_EDGE", "arg": [3, 0, 2, 1]},
            {"operator_id": "OP_HALT"}
        ]
    }
    resp = ipc.command("learn", json.dumps(pipeline_payload))
    print(f"[Bootstrap] Algorithm: {resp}")
