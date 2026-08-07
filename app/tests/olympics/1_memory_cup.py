#!/usr/bin/env python3
# app/tests/olympics/1_memory_cup.py
"""
    AGI OLYMPICS: MEMORY CUP
Уровень 2: Запоминание и Эффективность (Memorization & Baseline).

Тест проверяет:
1. Способность системы накапливать опыт (увеличение счетчиков LMDB).
2. Выживаемость долговременной памяти (Persistence) после полного Shutdown.
3. Согласованность метрик до и после загрузки данных.
"""
import sys
import time
from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[2]
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

from core.manager import EvolutionManager
from tools.knowledge_report import print_diff

def main():
    print("====================================================")
    print("     AGI OLYMPICS: MEMORY CUP (LEVEL 2: PERSISTENCE)     ")
    print("====================================================\n")

    manager = EvolutionManager()
    try:
        manager.initialize()
        core = manager.core_client

        print("[Memory Cup] Step 1: Initializing and getting baseline...")
        manager.execute_command("bootstrap")
        stats_before = core.get_stats()

        print("[Memory Cup] Step 2: Injecting episodic memory (simulating interaction)...")
        manager.execute_command("learn", "The server is running Ubuntu 22.04 LTS.")
        manager.execute_command("learn", "Ubuntu 22.04 LTS requires regular apt updates.")

        time.sleep(1) # Ждем, пока ядро асинхронно запишет данные

        stats_after = core.get_stats()
        print("\n[Memory Cup] Step 3: Knowledge Report Validation")
        print_diff("Knowledge Atoms", int(stats_before.get("atoms_total", 0)), int(stats_after.get("atoms_total", 0)))

        atoms_diff = int(stats_after.get("atoms_total", 0)) - int(stats_before.get("atoms_total", 0))
        assert atoms_diff > 0, "Memory did not grow after learning!"

        print("[Memory Cup] Step 4: Restarting core to verify persistence...")
    finally:
        manager.shutdown()

    time.sleep(1)

    # Новый инстанс менеджера (ядро запускается заново)
    manager2 = EvolutionManager()
    try:
        manager2.initialize()
        stats_restored = manager2.core_client.get_stats()

        restored_atoms = int(stats_restored.get("atoms_total", 0))
        after_atoms = int(stats_after.get("atoms_total", 0))

        print(f"[Memory Cup] Atoms before shutdown: {after_atoms}, after restart: {restored_atoms}")
        assert restored_atoms >= after_atoms, "Persistence failed! Memory lost after restart."

        print("\n====================================================")
        print("     MEMORY CUP: PASSED  ")
        print("====================================================")
    finally:
        manager2.shutdown()

if __name__ == "__main__":
    main()
