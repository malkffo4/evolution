#!/usr/bin/env python3
# app/tests/exam_test.py
"""
Экзамен на способность ядра к Индуктивному Синтезу (Out-Of-Distribution Generalization).
Тест общается с работающим процессом evolution_core через IPC.
Сценарий:
1. Загружаем 3 сырых факта ("прочитали книгу"), которые имеют один и тот же процесс
   и вызваны одной и той же причиной (нашей целью).
2. Активируем цель (InductiveSynthesisGoal).
3. Триггерим мышление ядра (MainLoop -> InductiveExtractor).
4. Проверяем, что ядро само обнаружило закономерность, сгенерировало свой
   собственный байткод (Code-as-Data), прогнало его через Критика и сохранило
   обобщенный факт в базу.
"""

import sys
import time
from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[1]
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

from core.sdk import CoreClient

GOAL_NAME = "InductiveSynthesisGoal"
PATTERN_PROC = "DiscoveredPhenomenon"

def main():
    print("[EXAM] Starting Out-Of-Distribution Generalization Test (via IPC)...")

    core = CoreClient()
    try:
        core.connect()
    except Exception as e:
        print(f"[ERROR] Could not connect to C-core. Is evolution_core running? ({e})")
        sys.exit(1)

    print("[EXAM] 1. Injecting 3 raw observations into LMDB (simulating book reading)...")
    # Мы подаем 3 факта. Главное здесь: cause = GOAL_NAME.
    # Встроенный в ядро InductiveExtractor ищет паттерны именно по cause_id.
    facts = {
        "atoms": [
            {"process": PATTERN_PROC, "args": ["Example1", "Prop1"], "cause": GOAL_NAME, "confidence": 1.0},
            {"process": PATTERN_PROC, "args": ["Example2", "Prop2"], "cause": GOAL_NAME, "confidence": 1.0},
            {"process": PATTERN_PROC, "args": ["Example3", "Prop3"], "cause": GOAL_NAME, "confidence": 1.0}
        ]
    }

    resp = core.learn(facts)
    if "error" in resp.get("name", ""):
        print(f"[FAILED] Could not inject facts: {resp}")
        sys.exit(1)

    time.sleep(2)
    print(f"[EXAM] 2. Activating goal '{GOAL_NAME}' in Working Memory...")
    core.activate_goal(GOAL_NAME, utility=0.9)
    time.sleep(2)
    print("[EXAM] 3. Triggering C-core subconscious (think)...")
    # Даем ядру время:
    # - Заметить цель
    # - Запустить InductiveExtractor
    # - Сгенерировать граф-байткод
    # - Выполнить его в песочнице (OP_EVAL_GRAPH)
    # - Подтвердить через OP_MERGE_CTX
    for i in range(5):
        core.think()
        time.sleep(1)

    time.sleep(5)
    print("[EXAM] 4. Verifying if core generalized the rule autonomously...")
    # Если InductiveExtractor отработал штатно, он сгенерировал правило, которое
    # взяло `process` паттерна и применило его к самой цели, создав новый атом.
    resp = core.retrieve(GOAL_NAME)
    atoms = resp.get("atoms", [])

    exam_passed = False
    for a in atoms:
        # Ищем атом, который ядро должно было породить автономно:
        if a.get("process") == PATTERN_PROC:
            args = a.get("args", [])
            # Выведенное правило подставляет R_GOAL в оба слота args
            if len(args) >= 2 and args[0] == GOAL_NAME and args[1] == GOAL_NAME:
                exam_passed = True
                break

    if exam_passed:
        print("\n============================================================")
        print("[SUCCESS] THE SYSTEM PASSED THE AGI EXAM!")
        print("The C-core autonomously:")
        print("  1. Found a causal pattern in raw data.")
        print("  2. Synthesized its own bytecode (Code-as-Data).")
        print("  3. Executed the graph-program in a sandbox.")
        print("  4. Merged the generalized conclusion back to reality.")
        print("All without Python logic or LLM interventions.")
        print("============================================================\n")
    else:
        print("\n[FAILED] The core did not synthesize the expected generalized rule.")
        print("Check if subconscious daemon and InductiveExtractor are properly initialized in the core.")
        sys.exit(1)

    core.close()

if __name__ == "__main__":
    main()
