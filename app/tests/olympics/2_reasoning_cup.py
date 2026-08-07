#!/usr/bin/env python3
# app/tests/olympics/2_reasoning_cup.py
"""
🏆 AGI OLYMPICS: REASONING CUP 🏆
Уровень 8: Создание нового знания (Out-Of-Distribution Generalization).

Тест проверяет способность C-ядра (InductiveExtractor + MetaCritic)
автономно обнаружить паттерн в сырых данных, синтезировать для него
собственный байткод и сохранить абстрактное правило в основную реальность
БЕЗ питоновского хардкода.
"""
import sys
import time
from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[2]
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

from core.manager import EvolutionManager

GOAL_NAME = "InductiveSynthesisGoal"
PATTERN_PROC = "DiscoveredPhenomenon"

def main():
    print("====================================================")
    print("     AGI OLYMPICS: REASONING CUP (LEVEL 8: NEW KNOWLEDGE)    ")
    print("====================================================\n")

    manager = EvolutionManager()
    try:
        manager.initialize()
        core = manager.core_client

        print("[Reasoning Cup] 1. Injecting raw observations (Simulating unsupervised discovery)...")
        facts = {
            "atoms": [
                {"process": PATTERN_PROC, "args": ["EntityA", "Trait1"], "cause": GOAL_NAME, "confidence": 1.0},
                {"process": PATTERN_PROC, "args": ["EntityB", "Trait2"], "cause": GOAL_NAME, "confidence": 1.0},
                {"process": PATTERN_PROC, "args": ["EntityC", "Trait3"], "cause": GOAL_NAME, "confidence": 1.0}
            ]
        }
        core.learn(facts)

        time.sleep(2)

        print("[Reasoning Cup] 2. Activating Subconscious Goal...")
        core.activate_goal(GOAL_NAME, utility=0.9)

        print("[Reasoning Cup] 3. Letting C-core Subconscious process it (Think)...")
        for _ in range(5):
            core.think()
            time.sleep(1)

        time.sleep(2)

        print("[Reasoning Cup] 4. Verifying if the Core generalized the rule autonomously...")
        resp = core.retrieve(GOAL_NAME)
        atoms = resp.get("atoms", [])

        passed = False
        for a in atoms:
            if a.get("process") == PATTERN_PROC:
                args = a.get("args", [])
                # Ядро должно было подставить саму цель в оба слота
                if len(args) >= 2 and args[0] == GOAL_NAME and args[1] == GOAL_NAME:
                    passed = True
                    break

        assert passed, "The core did not generalize the pattern."
        print("[Reasoning Cup] SUCCESS! The system autonomously synthesized a new rule from scattered data!")

        print("\n====================================================")
        print("     REASONING CUP: PASSED   ")
        print("====================================================")

    finally:
        manager.shutdown()

if __name__ == "__main__":
    main()
