#!/usr/bin/env python3
# app/tests/test_crash.py
# Crash Recovery:
# Скрипт test_crash.py должен запустить learn тяжелого пайплайна,
# сделать os.kill(core_pid, signal.SIGKILL), запустить ядро заново и через get_stats проверить,
# что LMDB не повреждена и кол-во атомов >= предыдущему дампу.
import subprocess
import sys
import time
import os
import signal
import threading
from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[1]
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

from core.sdk import CoreClient

CORE_BIN = APP_DIR.parent / "core" / "build" / "debug" / "bin" / "evolution_core"

def heavy_insert(atoms):
    try:
        core = CoreClient().connect()
        core.learn_atoms(atoms)
    except Exception:
        pass # Expected to die mid-flight

def main():
    print("[Crash] Starting core...")
    proc = subprocess.Popen([str(CORE_BIN)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)

    core = CoreClient().connect()
    baseline = core.get_stats()
    atoms_before = int(baseline.get("atoms_total", 0))
    print(f"[Crash] Baseline atoms: {atoms_before}")
    core.close()

    print("[Crash] Injecting heavy payload (2000 atoms)...")
    atoms = [{"process": "TEST_CRASH", "args": [f"Subj_{i}", "Obj"]} for i in range(2000)]

    # Запускаем в отдельном потоке
    t = threading.Thread(target=heavy_insert, args=(atoms,))
    t.start()

    # Даем время захватить лок LMDB
    time.sleep(0.2)

    print(f"[Crash] SENDING SIGKILL (kill -9) TO PID {proc.pid}!")
    os.kill(proc.pid, signal.SIGKILL)
    proc.wait()

    print("[Crash] Restarting core to verify LMDB integrity...")
    proc = subprocess.Popen([str(CORE_BIN)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)

    core = CoreClient().connect()
    recovered = core.get_stats()
    atoms_after = int(recovered.get("atoms_total", 0))

    print(f"[Crash] Recovered atoms: {atoms_after}")
    assert atoms_after >= atoms_before, "Database corrupted or lost data!"
    print("[Crash] PASSED - LMDB survived SIGKILL without corruption.")

    core._command("shutdown")
    proc.wait()

if __name__ == "__main__":
    main()
