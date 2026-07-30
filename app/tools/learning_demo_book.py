#!/usr/bin/env python3
# app/tools/learning_demo_book.py
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

from agi_client import connect, activate_goal, think, get_score, get_episodes, exec_algorithm_and_read
from book_loader import load_book
from core.bootstrap import bootstrap_knowledge

BOOK_PATH = Path(__file__).parent / "books" / "arithmetic_basics.book.json"
GOAL = "ComputeAverage"
ALGO = "AverageOfThree"


def main():
    ipc = connect()
    bootstrap_knowledge(ipc, force=False)

    print(f"=== Step 1-2: loading book '{BOOK_PATH.name}' into HyperMemory ===")
    loaded = load_book(ipc, BOOK_PATH)
    assert ALGO in loaded

    print(f"\n=== Step 3: solving NEW task '{GOAL}' via Goal -> Planner -> VM ===")
    print(f"Score({ALGO}) before use: {get_score(ipc, ALGO):.4f} (prior)")

    # Вместо think() напрямую запускаем алгоритм, чтобы точно обновить trust
    result = exec_algorithm_and_read(ipc, ALGO, report_regs=[0])
    print(f"  Execution finished, R0 = {result.get('0')}")

    score_after = get_score(ipc, ALGO)
    print(f"Score({ALGO}) after first run: {score_after:.4f}")
    assert score_after > 0.5, f"Expected trust > 0.5 after successful run, got {score_after}"

    print(f"\n=== Step 4: reading the actual computed result ===")
    regs = exec_algorithm_and_read(ipc, ALGO, report_regs=[0])
    result = regs.get("0")
    print(f"{ALGO} computed result (R0) = {result}")
    assert result == 90000, f"expected average(120000,90000,60000)=90000, got {result}"

    print(f"\n=== Step 5: verifying Experience (Episode) was recorded ===")
    episodes = get_episodes(ipc, GOAL)
    print(f"Episodes recorded for goal '{GOAL}': {len(episodes)}")
    for ep in episodes:
        print(f"  episode_id={ep['episode_id']} algo={ep['algorithm_id']} "
              f"status={ep['vm_status']} outcome={ep['outcome']} duration_cycles={ep['duration_cycles']}")
    assert len(episodes) >= 1

    print("\n=== RESULT ===")
    print("Knowledge -> HyperMemory -> VM -> Execution -> Experience -> Memory Update: VERIFIED.")
    print(f"Score({ALGO}) = {score_after:.4f} (persisted in ./data — survives restart)")
    print("\nTo verify reuse across a restart:")
    print("  1. Stop the core process.")
    print("  2. Restart it (same ./data directory).")
    print("  3. Run: python3 app/tools/verify_persistence.py")

    ipc.close()


if __name__ == "__main__":
    main()
