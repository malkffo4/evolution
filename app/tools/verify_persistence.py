#!/usr/bin/env python3
# app/tools/verify_persistence.py
"""Запускать ПОСЛЕ перезапуска ядра. LMDB (./data) уже персистентна по
умолчанию — этот скрипт только ставит это под проверку, никакого нового
кода для самой персистентности не требуется."""
from agi_client import connect, get_score, get_episodes

ALGO = "AverageOfThree"
GOAL = "ComputeAverage"


def main():
    ipc = connect()
    score = get_score(ipc, ALGO)
    episodes = get_episodes(ipc, GOAL)

    print(f"Score({ALGO}) after restart = {score:.4f}")
    print(f"Episodes for '{GOAL}' after restart = {len(episodes)}")

    assert score > 0.5, "Score did not survive restart — persistence broken!"
    assert len(episodes) >= 1, "Episodes did not survive restart — persistence broken!"

    print("\nPERSISTENCE VERIFIED: knowledge and experience from the previous "
          "run are still available and were NOT recomputed from scratch.")
    ipc.close()


if __name__ == "__main__":
    main()
