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
            {"int_consts": [10]},
            out_regs=[0]
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
            {"int_consts": [2, str(djb2_hash("VULNERABILITY"))]},
            in_regs=[1],
            out_regs=[0]
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

        print("[Composition Cup] 4. Verifying results in HyperMemory...")
        resp = core.retrieve("VULNERABILITY")
        atoms = resp.get("atoms", [])

        found_vuln = any(str(a.get("args", [])[0]) == "20" for a in atoms if a.get("process") == "VULNERABILITY")
        assert found_vuln, "Composition failed. Core did not assert VULNERABILITY(20, 20)."

        print("\n====================================================")
        print("     COMPOSITION CUP: PASSED (AUTONOMOUS CODE GEN)")
        print("====================================================")
    finally:
        manager.shutdown()

if __name__ == "__main__":
    main()
