#!/usr/bin/env python3
# app/tests/verify_persistence.py
"""Запускать ПОСЛЕ перезапуска ядра. LMDB (./data) уже персистентна по
умолчанию — этот скрипт только ставит это под проверку, никакого нового
кода для самой персистентности не требуется."""

import sys
from pathlib import Path

# ИСПРАВЛЕНИЕ: Добавляем корень 'app' в sys.path для корректных импортов
APP_DIR = Path(__file__).resolve().parents[1]
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

from tools.agi_client import connect, get_score, get_episodes

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
