#!/usr/bin/env python3
# загрузка начальных знаний
# app/core/bootstrap.py
import json, sys
from pathlib import Path
sys.path.append(str(Path(__file__).resolve().parents[1]))
from core.ipc import IPCClient

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
        {"process": "IS_A", "args": ["HAS_ALGORITHM", "GoalAlgorithmRelation"], "confidence": 1.0},
    ]
    resp = ipc.command("learn", json.dumps({"atoms": meta_atoms}))
    print(f"[Bootstrap] Meta-types: {resp}")

    test_data = {
        "nodes": [
            {"id": "A", "label": "A", "danger": 0.2, "utility": 0.9},
            {"id": "B", "label": "B", "danger": 0.1, "utility": 0.8}
        ],
        "edges": [
            {"source": "A", "target": "B", "relation": "CAUSES"}
        ]
    }
    ipc.command("learn", json.dumps(test_data))

    goal_activation = {
        "nodes": [{"id": "FindVulnerability", "label": "FindVulnerability", "danger": 0.9, "utility": 1.0}]
    }
    ipc.command("learn", json.dumps(goal_activation))

    # Цель
    goal_atoms = [
        {"process": "IS_A", "args": ["FindVulnerability", "Goal"], "confidence": 1.0},
        {"process": "HAS_ALGORITHM", "args": ["CheckEdgeAlgo", "FindVulnerability"], "confidence": 1.0},
    ]
    resp = ipc.command("learn", json.dumps({"atoms": goal_atoms}))
    print(f"[Bootstrap] Goal: {resp}")

    pipeline_payload = {
        "type": "pipeline",
        "algo_name": "CheckEdgeAlgo",
        "code": [
            {"operator_id": "check_cached_edge", "arg": [3, 0, 2, 1]},
            {"operator_id": "halt"}
        ],
        "constants": {
            "str_consts": ["A", "B", "CAUSES"]
        }
    }
    resp = ipc.command("learn", json.dumps(pipeline_payload))
    print(f"[Bootstrap] Algorithm: {resp}")
