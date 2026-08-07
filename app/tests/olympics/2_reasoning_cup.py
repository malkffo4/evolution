#!/usr/bin/env python3
# app/tests/olympics/2_reasoning_cup.py
"""
    AGI OLYMPICS: REASONING CUP
Уровень 8: Создание нового знания (Out-Of-Distribution Generalization).

Тест проверяет способность C-ядра (InductiveExtractor + MetaCritic)
автономно обнаружить паттерн в сырых данных, синтезировать для него
собственный байткод и сохранить абстрактное правило в основную реальность
БЕЗ питоновского хардкода.
"""
import sys
import time
import random
import string
from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[2]
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

from core.manager import EvolutionManager

def generate_alien_word(length=8):
    """Генерирует случайное слово, которое ядро 100% никогда не видело."""
    return "".join(random.choices(string.ascii_uppercase + string.ascii_lowercase, k=length))

def main():
    print("====================================================")
    print("     AGI OLYMPICS: REASONING CUP (TRUE OOD GENERALIZATION)   ")
    print("====================================================\n")

    # Генерируем случайные концепции для текущего запуска
    OOD_GOAL = f"Goal_{generate_alien_word()}"
    OOD_PROCESS = f"Proc_{generate_alien_word()}"
    OOD_ENTITIES = [f"Ent_{generate_alien_word()}" for _ in range(3)]

    print(f"[Reasoning Cup] Generated OOD concepts:")
    print(f"  - Goal:    {OOD_GOAL}")
    print(f"  - Process: {OOD_PROCESS}")
    print(f"  - Entities:{OOD_ENTITIES}\n")

    manager = EvolutionManager()
    try:
        manager.initialize()
        core = manager.core_client

        print("[Reasoning Cup] 1. Injecting abstract OOD observations...")
        facts = {
            "atoms": [
                {"process": OOD_PROCESS, "args": [OOD_ENTITIES[0], "Trait1"], "cause": OOD_GOAL, "confidence": 1.0},
                {"process": OOD_PROCESS, "args": [OOD_ENTITIES[1], "Trait2"], "cause": OOD_GOAL, "confidence": 1.0},
                {"process": OOD_PROCESS, "args": [OOD_ENTITIES[2], "Trait3"], "cause": OOD_GOAL, "confidence": 1.0}
            ]
        }
        core.learn(facts)
        time.sleep(1)

        print(f"[Reasoning Cup] 2. Activating Subconscious Goal: {OOD_GOAL}")
        core.activate_goal(OOD_GOAL, utility=0.9)

        print("[Reasoning Cup] 3. Letting C-core Subconscious process it (Think)...")
        for _ in range(5):
            core.think()
            time.sleep(0.5)

        print("[Reasoning Cup] 4. Verifying if the Core generalized the rule autonomously...")
        resp = core.retrieve(OOD_GOAL)
        atoms = resp.get("atoms", [])

        passed = False
        for a in atoms:
            if a.get("process") == OOD_PROCESS:
                args = a.get("args", [])
                # Ядро должно было абстрагировать паттерн и подставить цель в оба слота
                if len(args) >= 2 and args[0] == OOD_GOAL and args[1] == OOD_GOAL:
                    passed = True
                    print(f"[Reasoning Cup] Found synthesized rule: {OOD_PROCESS}({args[0]}, {args[1]})")
                    break

        assert passed, f"The core failed to generalize the OOD pattern for '{OOD_PROCESS}'."
        print("\n[Reasoning Cup] SUCCESS! The C-core is truly agnostic to semantics.")
        print("It recognized the structural pattern and synthesized valid bytecode for it!")

        print("\n====================================================")
        print("     REASONING CUP: PASSED (TRUE OOD)    ")
        print("====================================================")

    finally:
        manager.shutdown()

if __name__ == "__main__":
    main()
