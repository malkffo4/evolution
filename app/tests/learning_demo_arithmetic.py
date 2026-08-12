#!/usr/bin/env python3
# app/tests/learning_demo_arithmetic.py
"""
Первая end-to-end демонстрация замкнутого цикла обучения:
  Execution -> Observation -> Evaluation -> Credit Assignment -> Score -> Planner -> Better Action
"""
import json
import sys
import time
from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[1]
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

from core.ipc import IPCClient
from core.bootstrap import bootstrap_knowledge

DOMAIN_ALGORITHM = 1  # COGNITIVE_DOMAIN_ALGORITHM, см. knowledge/evaluation.h

GOAL_ID = "SolveArithmetic"
GOOD_ALGO = "GoodAddAlgo"
BAD_ALGO = "BadAddAlgo"

def learn(ipc, payload: dict) -> dict:
    resp = ipc.command("learn", json.dumps(payload))
    if resp.get("name") == "error":
        raise RuntimeError(f"learn failed: {resp.get('payload')}")
    return resp

def activate_goal(ipc, goal_id: str, utility: float = 0.9):
    learn(ipc, {"atoms": [
        {"process": "IS_A", "kind": "relation", "args": [goal_id, "Goal"], "confidence": 1.0}
    ]})
    learn(ipc, {"nodes": [
        {"id": goal_id, "label": goal_id, "danger": 0.1, "utility": utility}
    ]})

def learn_good_algo(ipc, name: str):
    payload = {
        "type": "pipeline",
        "algo_name": name,
        "code": [
            {"operator_id": "load_const", "arg": [1, 5, 0, 0, 0, 0]},
            {"operator_id": "load_const", "arg": [2, 10, 0, 0, 0, 0]},
            {"operator_id": "add",        "arg": [0, 1, 2, 0, 0, 0]},
            {"operator_id": "halt",       "arg": [0, 0, 0, 0, 0, 0]}
        ],
        "constants": {}
    }
    learn(ipc, payload)

def learn_bad_algo(ipc, name: str):
    payload = {
        "type": "pipeline",
        "algo_name": name,
        "code": [
            {"operator_id": "add", "arg": [0, 1, 2, 0, 0, 0]},
            {"operator_id": "halt", "arg": [0, 0, 0, 0, 0, 0]}
        ],
        "constants": {}
    }
    learn(ipc, payload)

def link_algorithm(ipc, algo_name: str, goal_id: str):
    learn(ipc, {"atoms": [
        {"process": "HAS_ALGORITHM", "kind": "relation", "args": [algo_name, goal_id], "confidence": 1.0}
    ]})

def get_score(ipc, subject: str, domain: int = DOMAIN_ALGORITHM) -> float:
    resp = ipc.request("get_score", {"subject": subject, "domain": domain})
    payload = resp.get("payload", {})
    if isinstance(payload, str):
        payload = json.loads(payload) if payload else {}
    return float(payload.get("score", 0.5))

def think(ipc, settle_sec: float = 0.3):
    ipc.command("think")
    time.sleep(settle_sec)

def main():
    ipc = IPCClient()
    ipc.connect()
    assert ipc.ping(), "Core not responding"

    bootstrap_knowledge(ipc, force=False)

    print(f"[demo] Goal={GOAL_ID}")
    activate_goal(ipc, GOAL_ID)

    print(f"[demo] Registering {BAD_ALGO} (designed to fail) and {GOOD_ALGO} (designed to succeed)")
    learn_bad_algo(ipc, BAD_ALGO)
    learn_good_algo(ipc, GOOD_ALGO)

    print(f"\n=== Phase 1: only {BAD_ALGO} is a candidate ===")
    link_algorithm(ipc, BAD_ALGO, GOAL_ID)

    for i in range(5):
        # Сбрасываем кулдаун ВНУТРИ цикла, чтобы планировщик брал цель каждый раз
        ipc.command("clear_cooldown", json.dumps({"goal": GOAL_ID}))
        activate_goal(ipc, GOAL_ID)

        think(ipc)
        print(f"  iter {i+1}: score({BAD_ALGO}) = {get_score(ipc, BAD_ALGO):.4f}")

    s_bad_final = get_score(ipc, BAD_ALGO)
    s_good_initial = get_score(ipc, GOOD_ALGO)

    print(f"\n[demo] {BAD_ALGO} degraded to {s_bad_final:.4f} (prior was 0.5000)")
    print(f"[demo] {GOOD_ALGO} still at prior: {s_good_initial:.4f} (never executed yet)")

    assert s_bad_final < 0.5, "Bad algorithm should have degraded below prior"

    print(f"\n=== Phase 2: {GOOD_ALGO} joins as a second candidate ===")
    link_algorithm(ipc, GOOD_ALGO, GOAL_ID)

    for i in range(5):
        # Аналогично поддерживаем цель активной и доступной на каждом шаге
        ipc.command("clear_cooldown", json.dumps({"goal": GOAL_ID}))
        activate_goal(ipc, GOAL_ID)

        think(ipc)
        s_good = get_score(ipc, GOOD_ALGO)
        s_bad = get_score(ipc, BAD_ALGO)
        print(f"  iter {i+1}: score({GOOD_ALGO})={s_good:.4f}  score({BAD_ALGO})={s_bad:.4f}")

    s_good_final = get_score(ipc, GOOD_ALGO)
    print(f"\n[demo] RESULT: score({GOOD_ALGO}) = {s_good_final:.4f} (started at 0.5)")

    assert s_good_final > 0.5, "Good algorithm should have improved above prior"
    assert s_good_final > s_bad_final, "Planner should now clearly prefer the good algorithm"

    print("\n[demo] Learning loop verified end-to-end:")
    print("  Execution -> Observation -> Evaluation -> Credit Assignment -> Score -> Planner")
    print("  Planner now systematically prefers GoodAddAlgo based on experience, not hardcoding.")

    ipc.close()

if __name__ == "__main__":
    main()
