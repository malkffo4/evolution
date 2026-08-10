#!/usr/bin/env python3
# app/tests/olympics/2_reasoning_cup.py
import sys
import time
import random
import string
from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[2]
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

from core.manager import EvolutionManager

def gen_word(): return "".join(random.choices(string.ascii_letters, k=6))

def main():
    print("====================================================")
    print("     AGI OLYMPICS: REASONING CUP (TRUE OOD GENERALIZATION)   ")
    print("====================================================\n")

    # УНИКАЛЬНАЯ ЦЕЛЬ: изолирует этот запуск от мусора в LMDB из прошлых тестов
    OOD_GOAL = f"InductiveSynthesisGoal_{gen_word()}"
    OOD_PROCESS = f"Proc_{gen_word()}"
    OOD_ENTITIES = [f"Ent_{gen_word()}" for _ in range(3)]

    print(f"[Reasoning Cup] Generated OOD concepts:")
    print(f"  - Goal:    {OOD_GOAL}")
    print(f"  - Process: {OOD_PROCESS}")

    manager = EvolutionManager()
    try:
        manager.initialize()
        core = manager.core_client

        print("[Reasoning Cup] 1. Injecting abstract OOD observations...")
        facts = {
            "atoms": [
                {"process": OOD_PROCESS, "args": [OOD_ENTITIES[0], "Trait1"], "cause": OOD_GOAL, "confidence": 1.0},
                {"process": OOD_PROCESS, "args": [OOD_ENTITIES[1], "Trait2"], "cause": OOD_GOAL, "confidence": 1.0},
                {"process": OOD_PROCESS, "args": [OOD_ENTITIES[2], "Trait3"], "cause": OOD_GOAL, "confidence": 1.0},
                # Явно связываем Индуктивный Экстрактор с нашей новой целью
                {"process": "IS_A", "kind": "relation", "args": [OOD_GOAL, "Goal"], "confidence": 1.0},
                {"process": "HAS_ALGORITHM", "kind": "relation", "args": ["InductiveExtractor", OOD_GOAL], "confidence": 1.0}
            ]
        }
        core.learn(facts)
        time.sleep(1)

        print(f"[Reasoning Cup] 2. Activating Subconscious Goal: {OOD_GOAL}")
        core.clear_cooldown(OOD_GOAL)
        core.activate_goal(OOD_GOAL, utility=0.9)

        print("[Reasoning Cup] 3. Letting C-core Subconscious process it (Think)...")
        core.think()

        # Ждем реального эпизода выполнения, а не просто спим!
        core.wait_for_episode(OOD_GOAL, timeout_sec=8.0)

        print("[Reasoning Cup] 4. Verifying if the Core generalized the rule autonomously...")
        resp = core.retrieve(OOD_GOAL)
        atoms = resp.get("atoms", [])

        passed = False
        for a in atoms:
            if a.get("process") == OOD_PROCESS:
                args = a.get("args", [])
                if len(args) >= 2 and str(args[0]) == OOD_GOAL and str(args[1]) == OOD_GOAL:
                    passed = True
                    print(f"[Reasoning Cup] Found synthesized rule: {OOD_PROCESS}({args[0]}, {args[1]})")
                    break

        assert passed, f"The core failed to generalize the OOD pattern for '{OOD_PROCESS}'."
        print("\n[Reasoning Cup] SUCCESS! The C-core is truly agnostic to semantics.")

    finally:
        manager.shutdown()

if __name__ == "__main__":
    main()
