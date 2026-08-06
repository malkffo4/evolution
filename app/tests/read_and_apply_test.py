#!/usr/bin/env python3
# app/tests/read_and_apply_test.py
"""
E2E: Прочитал -> Осознал -> Применил.

1. Текстовое описание алгоритма -> LLM переводит в маленький арифметический
   DSL (load_const/add/sub/mul/div), не в свободный текст.
2. DSL компилируется в тот же Pipeline-JSON формат, что book_loader.py
   (code[] + constants{int_consts}) -> IPC "learn" (type=pipeline).
3. HAS_ALGORITHM(algo, goal) регистрируется тем же путём, что bootstrap.py.
4. Цель активируется, применяется к НОВЫМ входным данным через синхронный
   execute_op(op=exec_algorithm, regs, report_regs) (Задача 4.1).
5. assert на числовой результат.
"""
import json
import re
import sys
from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[1]
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

from core.ipc import IPCClient
from core.llm import LLMClient

ALGO_NAME = "CircleAreaSkill"
GOAL_NAME = "ComputeCircleArea"

DSL_PROMPT = """Ты — компилятор алгоритмов NeuroCore. Переведи описание в
последовательность арифметических шагов ФИКСИРОВАННОЙ схемы. Доступные
операции: load_const (загрузить число в регистр), add, sub, mul, div
(операция над двумя регистрами, результат в третий). Регистры — целые
числа 1..8. Вход (радиус) уже лежит в регистре 1 — используй его, не
загружай радиус через load_const.

Верни СТРОГО JSON:
{{"steps": [{{"op":"load_const","dst":2,"value":314}},
            {{"op":"mul","dst":3,"a":1,"b":1}},
            {{"op":"mul","dst":4,"a":2,"b":3}},
            {{"op":"load_const","dst":5,"value":100}},
            {{"op":"div","dst":0,"a":4,"b":5}}],
  "output_register": 0}}

output_register — регистр с финальным результатом (по конвенции R0).

Описание алгоритма:
\"\"\"{description}\"\"\"
"""

DESCRIPTION = (
    "Чтобы вычислить площадь круга в целых числах, нужно умножить радиус сам на себя (возвести в квадрат), "
    "затем умножить на 314 (приближенное Пи * 100) и разделить результат на 100."
)


def _parse_json(raw: str):
    raw = re.sub(r"^```(json)?", "", raw.strip()).strip()
    raw = re.sub(r"```$", "", raw).strip()
    return json.loads(raw)


def compile_dsl_to_pipeline(dsl: dict) -> dict:
    """DSL -> book_loader.py-совместимый Pipeline JSON."""
    code, int_consts = [], []
    op_map = {"add": "add", "sub": "sub", "mul": "mul", "div": "div"}

    for step in dsl["steps"]:
        if step["op"] == "load_const":
            const_idx = len(int_consts)
            int_consts.append(int(step["value"])) # Принудительно приводим к int
            code.append({"operator_id": "load_const", "arg": [step["dst"], const_idx, 0, 0, 0, 0]})
        elif step["op"] in op_map:
            code.append({"operator_id": op_map[step["op"]],
                         "arg": [step["dst"], step["a"], step["b"], 0, 0, 0]})
        else:
            raise ValueError(f"unsupported DSL op: {step['op']}")

    out_reg = dsl["output_register"]
    if out_reg != 0:
        code.append({"operator_id": "move", "arg": [0, out_reg, 0, 0, 0, 0]})
    code.append({"operator_id": "halt", "arg": [0, 0, 0, 0, 0, 0]})

    return {"type": "pipeline", "algo_name": ALGO_NAME, "code": code,
            # Избавляемся от float_consts полностью
            "constants": {"int_consts": int_consts}}

def main():
    ipc = IPCClient()
    ipc.connect()
    assert ipc.ping(), "Core not responding"
    llm = LLMClient()

    print("=== Шаг 1: 'Прочитал' — LLM переводит описание в DSL ===")
    try:
        raw = llm.query(DSL_PROMPT.format(description=DESCRIPTION), json_mode=True)
        dsl = _parse_json(raw)
        print(f"  DSL: {dsl}")
    except Exception as e:
        print(f"  LLM query failed: {e}", file=sys.stderr)
        ipc.close()
        return

    print("\n=== Шаг 2: 'Осознал' — компиляция DSL в исполняемый Pipeline ===")
    pipeline_payload = compile_dsl_to_pipeline(dsl)
    resp = ipc.command("learn", json.dumps(pipeline_payload))
    assert resp.get("name") != "error", f"pipeline learn failed: {resp}"
    print(f"  зарегистрирован алгоритм '{ALGO_NAME}': {resp.get('payload')}")

    link = {"atoms": [
        {"process": "IS_A", "kind": "relation", "args": [GOAL_NAME, "Goal"], "confidence": 1.0},
        {"process": "HAS_ALGORITHM", "kind": "relation", "args": [ALGO_NAME, GOAL_NAME], "confidence": 1.0},
    ]}
    resp = ipc.command("learn", json.dumps(link))
    assert resp.get("name") != "error", f"link failed: {resp}"

    print("\n=== Шаг 3: 'Применил' — синхронный вызов с НОВЫМ радиусом=10 ===")
    exec_payload = {
        "op": "exec_algorithm",
        "regs": {"1": 10, "5": ALGO_NAME},   # R1=радиус (вход), R5=algo_id (хэшируется на сервере)
        "report_regs": [0],
    }
    resp = ipc.command("execute_op", json.dumps(exec_payload))
    payload = resp.get("payload", {})
    if isinstance(payload, str):
        payload = json.loads(payload) if payload else {}
    assert "error" not in payload, f"exec_algorithm failed: {payload}"

    result = payload["reported_regs"]["0"]
    expected = round(3.14159 * 10 * 10)  # ~314
    print(f"  R0 (площадь круга, r=10) = {result}, ожидалось ~{expected}")

    assert abs(result - expected) <= 1, f"expected ~{expected}, got {result}"

    print("\n[E2E] Прочитал -> Осознал -> Применил: ПРОЙДЕНО.")
    ipc.close()


if __name__ == "__main__":
    main()
