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

from core.sdk import CoreClient

ALGO = "AverageOfThree"
GOAL = "ComputeAverage"


def main():
    core = CoreClient()
    core.connect()

    score = core.get_score(ALGO)
    episodes = core.get_episodes(GOAL)

    print(f"Score({ALGO}) after restart = {score:.4f}")
    print(f"Episodes for '{GOAL}' after restart = {len(episodes)}")

    assert score > 0.5, "Score did not survive restart — persistence broken!"
    assert len(episodes) >= 1, "Episodes did not survive restart — persistence broken!"

    print("\nPERSISTENCE VERIFIED: knowledge and experience from the previous "
          "run are still available and were NOT recomputed from scratch.")
    core.close()


if __name__ == "__main__":
    main()
