#!/usr/bin/env python3
# app/tests/olympics/run_olympics.py
"""
Запускает все кубки AGI Olympics по очереди.
"""
import subprocess
import sys
from pathlib import Path

def run_test(script_path: Path):
    print(f"\n🏃 Running {script_path.name}...")
    result = subprocess.run([sys.executable, str(script_path)])
    if result.returncode != 0:
        print(f"❌ {script_path.name} FAILED!")
        sys.exit(result.returncode)

def main():
    print("====================================================")
    print("         NEUROCORE AGI OLYMPICS      ")
    print("====================================================")

    tests_dir = Path(__file__).parent

    cups = [
        "1_memory_cup.py",
        "2_reasoning_cup.py",
        "3_book_learning_cup.py"
    ]

    for cup in cups:
        script = tests_dir / cup
        if script.exists():
            run_test(script)
        else:
            print(f"⚠️ Test not found: {script}")
            sys.exit(1)

    print("\n====================================================")
    print("         ALL OLYMPIC CUPS PASSED SUCCESSFULLY!       ")
    print("====================================================")

if __name__ == "__main__":
    main()
