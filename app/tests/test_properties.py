#!/usr/bin/env python3
# app/tests/test_properties.py
# Property-based:
# В test_properties.py через Python SDK сгенерируй случайные числа,
# запакуй их в пайплайны "A+B" и "B+A", отправь в ядро и сделай assert res_A == res_B.
import sys
import random
from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[1]
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

from core.sdk import CoreClient

def main():
    core = CoreClient().connect()

    a = random.randint(1, 1000)
    b = random.randint(1, 1000)

    # Пайплайн 1: A + B
    core.learn_pipeline("AlgoA", [
        {"operator_id": "load_const", "arg": [1, 0, 0, 0, 0, 0]},
        {"operator_id": "load_const", "arg": [2, 1, 0, 0, 0, 0]},
        {"operator_id": "add",        "arg": [0, 1, 2, 0, 0, 0]},
        {"operator_id": "halt",       "arg": [0, 0, 0, 0, 0, 0]}
    ], {"int_consts": [a, b]})

    # Пайплайн 2: B + A (обратный порядок констант)
    core.learn_pipeline("AlgoB", [
        {"operator_id": "load_const", "arg": [1, 0, 0, 0, 0, 0]},
        {"operator_id": "load_const", "arg": [2, 1, 0, 0, 0, 0]},
        {"operator_id": "add",        "arg": [0, 1, 2, 0, 0, 0]},
        {"operator_id": "halt",       "arg": [0, 0, 0, 0, 0, 0]}
    ], {"int_consts": [b, a]})

    res_a = core.exec_algorithm("AlgoA", report_regs=[0]).get("0")
    res_b = core.exec_algorithm("AlgoB", report_regs=[0]).get("0")

    print(f"Testing Commutativity: {a} + {b} == {b} + {a}")
    print(f"Res A: {res_a}, Res B: {res_b}")

    assert res_a == res_b, "Commutativity property failed!"
    print("[Properties] PASSED")

    core.close()

if __name__ == "__main__":
    main()
