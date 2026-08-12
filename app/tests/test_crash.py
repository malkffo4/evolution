#!/usr/bin/env python3
# app/tests/test_crash.py
# Crash Recovery:
# Скрипт test_crash.py должен запустить learn тяжелого пайплайна,
# сделать os.kill(core_pid, signal.SIGKILL), запустить ядро заново и через get_stats проверить,
# что LMDB не повреждена и кол-во атомов >= предыдущему дампу.
import subprocess
import time
import os
import signal
import threading

import sys
from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[1]
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

from core.sdk import CoreClient
from core.manager import EvolutionManager

def heavy_insert(atoms):
    try:
        core = CoreClient().connect()
        core.learn_atoms(atoms)
    except Exception:
        pass # Ожидаемо упадет при SIGKILL

def main():
    print("[Crash] Connecting to existing core...")
    manager = EvolutionManager()

    # Пытаемся подключиться к существующему ядру
    core = CoreClient().connect()

    baseline = core.get_stats()
    atoms_before = int(baseline.get("atoms_total", 0))
    print(f"[Crash] Baseline atoms: {atoms_before}")
    core.close()

    print("[Crash] Injecting heavy payload (2000 atoms)...")
    atoms = [{"process": "TEST_CRASH", "args": [f"Subj_{i}", "Obj"]} for i in range(2000)]

    t = threading.Thread(target=heavy_insert, args=(atoms,))
    t.start()
    time.sleep(0.2)

    # ЖЕСТКО убиваем ядро через встроенный механизм очистки
    print(f"[Crash] SENDING SIGKILL (kill -9) to evolution_core!")
    manager._cleanup_orphans()

    print("[Crash] Restarting core via Manager to verify LMDB integrity...")
    manager.start_core()
    manager.wait_core()

    core = CoreClient().connect()
    recovered = core.get_stats()
    atoms_after = int(recovered.get("atoms_total", 0))

    print(f"[Crash] Recovered atoms: {atoms_after}")
    assert atoms_after >= atoms_before, "Database corrupted or lost data!"
    print("[Crash] PASSED - LMDB survived SIGKILL without corruption.")

    # ФИКС: Корректно закрываем это ядро, чтобы не плодить зомби с битыми FD
    core._command("shutdown")
    core.close()
    manager._stop_research_worker(force=True)
    manager._cleanup_orphans()

if __name__ == "__main__":
    main()
