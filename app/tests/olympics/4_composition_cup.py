#!/usr/bin/env python3
# app/tests/olympics/4_composition_cup.py
import sys, time
import random, string
from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[2]
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

from core.manager import EvolutionManager
from core.sdk import djb2_hash

def gen_word(): return "".join(random.choices(string.ascii_letters, k=6))

IP_CONST = 10
TARIFF_CONST = 2

def has_algorithm_link(core, goal: str) -> bool:
    resp = core.retrieve(goal)
    for a in resp.get("atoms", []):
        if a.get("process") == "HAS_ALGORITHM" and goal in a.get("args", []):
            return True
    return False

def main():
    print("====================================================")
    print("     AGI OLYMPICS: COMPOSITION CUP (LEVEL 9: ZERO-SHOT)")
    print("====================================================\n")

    manager = EvolutionManager()
    try:
        manager.initialize()
        core = manager.core_client

        # Динамическая цель, чтобы LMDB не запоминал успех прошлых запусков
        GOAL = f"FindVulnFromLogGoal_{gen_word()}"

        print("[Composition Cup] 1. Injecting fundamental skills (A and B)...")
        core.learn_pipeline(
            "ExtractIP",
            [
                {"operator_id": "load_const", "arg": [0, 0, 0, 0, 0, 0]},
                {"operator_id": "halt"}
            ],
            {"int_consts": [IP_CONST]},
            out_regs=[0],
        )

        core.learn_pipeline(
            "ScanIP",
            [
                {"operator_id": "load_const", "arg": [2, 0, 0, 0, 0, 0]},
                {"operator_id": "mul", "arg": [0, 1, 2, 0, 0, 0]},
                {"operator_id": "load_const", "arg": [3, 1, 0, 0, 0, 0]},
                {"operator_id": "assert", "arg": [3, 0, 0, 4, 0, 0]},
                {"operator_id": "halt"}
            ],
            {"int_consts": [TARIFF_CONST, str(djb2_hash("VULNERABILITY"))]},
            in_regs=[1],
            out_regs=[0],
        )

        facts = {"atoms": [
            {"process": "PRODUCES", "kind": "relation", "args": ["ExtractIP", "IP_Address"], "confidence": 1.0},
            {"process": "REQUIRES", "kind": "relation", "args": ["ScanIP", "IP_Address"], "confidence": 1.0},
            {"process": "PRODUCES", "kind": "relation", "args": ["ScanIP", GOAL], "confidence": 1.0},
            {"process": "IS_A", "kind": "relation", "args": [GOAL, "Goal"], "confidence": 1.0}
        ]}
        core.learn(facts)
        time.sleep(0.5)

        print("[Composition Cup] 2. Subconscious builds the plan (ZeroShotComposer runs)...")
        core.activate_goal(GOAL, utility=0.95)
        for _ in range(5):
            core.think()
            time.sleep(0.5)

        print("[Composition Cup] 3. Executing the autonomously composed plan...")
        core.clear_cooldown(GOAL)
        core.activate_goal(GOAL, utility=0.95)
        for _ in range(5):
            core.think()
            time.sleep(0.5)

        expected = IP_CONST * TARIFF_CONST  # семантика, а не хардкод "20"

        print("[Composition Cup] 4. Verifying results in HyperMemory...")
        resp = core.retrieve("VULNERABILITY")
        atoms = resp.get("atoms", [])

        found_vuln = any(
            a.get("process") == "VULNERABILITY"
            and len(a.get("args", [])) >= 2
            and str(a["args"][0]) == str(expected)
            and str(a["args"][1]) == str(expected)
            for a in atoms
        )
        composed = has_algorithm_link(core, GOAL)

        assert composed, f"ZeroShotComposer не связал новый алгоритм с {GOAL}"
        assert found_vuln, f"Ожидался VULNERABILITY({expected}, {expected}), получено: {atoms}"

        print(f"[Composition Cup] composed={composed} vuln_value={expected} -> OK")
    finally:
        manager.shutdown()

if __name__ == "__main__":
    main()
