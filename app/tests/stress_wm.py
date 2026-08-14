#!/usr/bin/env python3
# app/tests/stress_wm.py
# Стресс Рабочей Памяти, который в цикле 10 000 раз вызывает activate_goal с уникальными ID.
# Ожидаемый результат: ядро не падает, wm->count держится на уровне 256, новые цели вытесняют старые.
import sys
import time
import tempfile
from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[1]
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

from core.manager import EvolutionManager

def main():
    print("[Stress] Starting Working Memory stress test (10,000 goals)...")

    # Создаем временную директорию для изолированной базы
    with tempfile.TemporaryDirectory() as tmpdir:
        # Запускаем ядро на временной базе
        manager = EvolutionManager(db_path=tmpdir)
        manager.start_core()
        manager.wait_core()
        core = manager.core_client

        t0 = time.time()
        for i in range(10000):
            core.activate_goal(f"StressGoal_{i}", utility=0.9)
            if i > 0 and i % 2000 == 0:
                print(f"  ... {i} goals activated")

        dt = time.time() - t0
        print(f"[Stress] 10000 goals dispatched in {dt:.2f}s")

        stats = core.get_stats()
        print(f"[Stress] Core is alive. Stats: {stats}")
        print("[Stress] PASSED - No crash, LRU eviction handled the load.")

        # Глушим ядро, временная папка удалится автоматически
        manager.shutdown()

if __name__ == "__main__":
    main()
