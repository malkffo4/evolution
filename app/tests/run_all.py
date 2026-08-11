#!/usr/bin/env python3
# app/tests/run_all.py
import subprocess
import sys
from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[1]
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

from core.manager import EvolutionManager

def run_pytest():
    print("=" * 60)
    print("   ФАЗА 1: АРХИТЕКТУРНЫЕ КОНТРАКТЫ (PYTEST)")
    print("=" * 60)

    tests_e2e_dir = Path(__file__).parent / "e2e"
    if not tests_e2e_dir.exists():
        print(f"[WARN] Директория с Pytest контрактами не найдена: {tests_e2e_dir}")
        return

    result = subprocess.run(["pytest", str(tests_e2e_dir), "-v"])
    if result.returncode != 0:
        print("\n[ERROR] Контракты нарушены! Тестирование остановлено.")
        sys.exit(result.returncode)
    print("[OK] Архитектурные контракты подтверждены.\n")

def run_script(script_path: Path):
    print(f"-> Запуск {script_path.name}...")

    # ФИКС: Перед каждым тестом гарантируем, что ядро живо и здорово!
    manager = EvolutionManager()
    if not manager.is_core_responding():
        manager.start_core()
        manager.wait_core()

    result = subprocess.run([sys.executable, str(script_path)])
    if result.returncode != 0:
        print(f"[ERROR] Скрипт {script_path.name} УПАЛ!")
        sys.exit(result.returncode)

def main():
    print("====================================================")
    print("         NEUROCORE E2E & COGNITIVE TEST SUITE       ")
    print("====================================================")

    run_pytest()

    print("=" * 60)
    print("   ФАЗА 2: КОГНИТИВНЫЕ КУБКИ И ДЕМОНСТРАЦИИ")
    print("=" * 60)

    tests_dir = Path(__file__).parent
    olympics_dir = tests_dir / "olympics"

    scripts_to_run = [
        tests_dir / "test_properties.py",
        tests_dir / "test_crash.py",
        tests_dir / "stress_wm.py",
        tests_dir / "test_semantic_fallback.py",
        tests_dir / "exam_test.py",
        tests_dir / "learning_demo_book.py",
        tests_dir / "learning_demo_arithmetic.py",
        tests_dir / "zero_shot_composition_demo.py",
        olympics_dir / "1_memory_cup.py",
        olympics_dir / "2_reasoning_cup.py",
        olympics_dir / "3_book_learning_cup.py",
        olympics_dir / "4_composition_cup.py"
    ]

    for script in scripts_to_run:
        if script.exists():
            run_script(script)
        else:
            print(f"[WARN] Скрипт не найден, пропускаем: {script.name}")

    print("\n====================================================")
    print("         ВСЕ ТЕСТЫ УСПЕШНО ПРОЙДЕНЫ!       ")
    print("====================================================")

if __name__ == "__main__":
    main()
