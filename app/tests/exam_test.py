#!/usr/bin/env python3
# app/tests/exam_test.py
import random
import string
import sys
import time
from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[1]
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

from core.sdk import CoreClient

def gen_word(): return "".join(random.choices(string.ascii_letters, k=6))

GOAL_NAME = f"InductiveSynthesisGoal_{gen_word()}"
PATTERN_PROC = f"DiscoveredPhenomenon_{gen_word()}"

def main():
    print("[EXAM] Starting Out-Of-Distribution Generalization Test (via IPC)...")
    core = CoreClient()
    try:
        core.connect()
    except Exception as e:
        print(f"[ERROR] Could not connect to C-core. ({e})")
        sys.exit(1)

    print("[EXAM] 1. Injecting 3 raw observations into LMDB...")
    facts = {
        "atoms": [
            {"process": PATTERN_PROC, "args": ["Example1", "Prop1"], "cause": GOAL_NAME, "confidence": 1.0},
            {"process": PATTERN_PROC, "args": ["Example2", "Prop2"], "cause": GOAL_NAME, "confidence": 1.0},
            {"process": PATTERN_PROC, "args": ["Example3", "Prop3"], "cause": GOAL_NAME, "confidence": 1.0},
            {"process": "IS_A", "kind": "relation", "args": [GOAL_NAME, "Goal"], "confidence": 1.0},
            {"process": "HAS_ALGORITHM", "kind": "relation", "args": ["InductiveExtractor", GOAL_NAME], "confidence": 1.0}
        ]
    }
    core.learn(facts)
    time.sleep(1)

    print(f"[EXAM] 2. Activating goal '{GOAL_NAME}' in Working Memory...")
    core.clear_cooldown(GOAL_NAME)
    core.activate_goal(GOAL_NAME, utility=0.9)

    print("[EXAM] 3. Triggering C-core subconscious (think)...")
    core.think()

    # Ждем завершения эпизода!
    ep = core.wait_for_episode(GOAL_NAME, timeout_sec=8.0)
    if ep:
        print(f"[EXAM] Episode recorded: status={ep.vm_status} outcome={ep.outcome}")

    print("[EXAM] 4. Verifying if core generalized the rule autonomously...")
    resp = core.retrieve(GOAL_NAME)
    atoms = resp.get("atoms", [])

    exam_passed = False
    for a in atoms:
        if a.get("process") == PATTERN_PROC:
            args = a.get("args", [])
            if len(args) >= 2 and str(args[0]) == GOAL_NAME and str(args[1]) == GOAL_NAME:
                exam_passed = True
                break

    if exam_passed:
        print("\n============================================================")
        print("[SUCCESS] THE SYSTEM PASSED THE AGI EXAM!")
        print("============================================================\n")
    else:
        print("\n[FAILED] The core did not synthesize the expected generalized rule.")
        sys.exit(1)
    core.close()

if __name__ == "__main__":
    main()
