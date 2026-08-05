#!/usr/bin/env python3
# app/tests/learning_demo_book.py
"""
Knowledge -> HyperMemory -> VM -> Execution -> Experience -> Memory Update
-> повторное использование знаний.

1. Загрузить книгу алгоритмов.
2. Знания + исполняемые пайплайны сохраняются в HyperMemory автоматически.
3. Решить НОВУЮ задачу (ComputeAverage) через реальный
   Goal -> Planner -> VM цикл (vm_op_evaluate_goals).
4. Прочитать фактический числовой результат через execute_op.
5. Проверить, что опыт сохранён: Score вырос, Episode записан.

Персистентность между перезапусками проверяется verify_persistence.py —
LMDB переживает рестарт процесса без какого-либо дополнительного кода.
"""
from pathlib import Path
import sys

APP_DIR = Path(__file__).resolve().parents[1]
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

from core.sdk import CoreClient
from core.bootstrap import bootstrap_knowledge
from tools.book_loader import load_book

BOOK_PATH = Path(__file__).parent / "books" / "arithmetic_basics.book.json"
GOAL = "ComputeAverage"
ALGO = "AverageOfThree"

def main():
    core = CoreClient()
    core.connect()

    bootstrap_knowledge(core._ipc, force=False)

    print(f"=== Step 1-2: loading book '{BOOK_PATH.name}' into HyperMemory ===")
    loaded = load_book(core._ipc, BOOK_PATH)
    assert ALGO in loaded

    print(f"\n=== Step 3: solving NEW task '{GOAL}' via Goal -> Planner -> VM ===")
    print(f"Score({ALGO}) before use: {core.get_score(ALGO):.4f} (prior — never executed yet)")

    core.activate_goal(GOAL)

    for i in range(3):
        core.think()
        print(f"  think() iter {i+1}: score({ALGO}) = {core.get_score(ALGO):.4f}")

    score_after = core.get_score(ALGO)
    assert score_after > 0.5, "Algorithm learned from the book should now be trusted above prior"

    print(f"\n=== Step 4: reading the actual computed result ===")
    regs = core.exec_algorithm(ALGO, report_regs=[0])
    result = regs.get("0")
    print(f"{ALGO} computed result (R0) = {result}")
    assert result == 90000, f"expected average(120000,90000,60000)=90000, got {result}"

    print(f"\n=== Step 5: verifying Experience (Episode) was recorded ===")
    episodes = core.get_episodes(GOAL)
    print(f"Episodes recorded for goal '{GOAL}': {len(episodes)}")
    for ep in episodes:
        print(f"  episode_id={ep.episode_id} algo={ep.algorithm_id} "
              f"status={ep.vm_status} outcome={ep.outcome} duration_cycles={ep.duration_cycles}")

    assert len(episodes) >= 1

    print("\n=== RESULT ===")
    print("Knowledge -> HyperMemory -> VM -> Execution -> Experience -> Memory Update: VERIFIED.")
    print(f"Score({ALGO}) = {score_after:.4f} (persisted in ./data — survives restart)")
    print("\nTo verify reuse across a restart:")
    print("  1. Stop the core process.")
    print("  2. Restart it (same ./data directory).")
    print("  3. Run: python3 app/tests/verify_persistence.py")

    core.close()

if __name__ == "__main__":
    main()
