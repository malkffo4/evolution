#!/usr/bin/env python3
# app/tests/zero_shot_composition_demo.py
"""Ядро само собирает B∘A из двух блоков, никогда не связанных с целью,
используя только PRODUCES/REQUIRES в графе. Ноль Python-хардкода "как
решить именно эту задачу"."""
import json
import sys
import time
from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[1]
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

from core.ipc import IPCClient
from core.bootstrap import bootstrap_knowledge

GOAL = "ComputeShippingCost"
ALGO_A = "FetchPackageWeight"    # PRODUCES(A, PackageWeight)
ALGO_B = "PriceByWeight"         # REQUIRES(B, PackageWeight), PRODUCES(B, GOAL)
RESOURCE_X = "PackageWeight"


def learn(ipc, payload):
    resp = ipc.command("learn", json.dumps(payload))
    if resp.get("name") == "error":
        raise RuntimeError(f"learn failed: {resp.get('payload')}")
    return resp


def register_building_blocks(ipc):
    learn(ipc, {"type": "pipeline", "algo_name": ALGO_A, "code": [
        {"operator_id": "load_const", "arg": [0, 0, 0, 0, 0, 0]},   # R0 = "вес" = 42
        {"operator_id": "halt", "arg": [0, 0, 0, 0, 0, 0]},
    ], "constants": {"int_consts": [42]}})

    learn(ipc, {"type": "pipeline", "algo_name": ALGO_B, "code": [
        {"operator_id": "load_const", "arg": [1, 0, 0, 0, 0, 0]},   # R1 = тариф = 8
        {"operator_id": "add", "arg": [0, 0, 1, 0, 0, 0]},          # R0 += R1 (читает результат A!)
        {"operator_id": "halt", "arg": [0, 0, 0, 0, 0, 0]},
    ], "constants": {"int_consts": [8]}})

    # Единственное, что делает композицию возможной — HAS_ALGORITHM(*, GOAL) НЕ создаём.
    learn(ipc, {"atoms": [
        {"process": "IS_A", "kind": "relation", "args": [GOAL, "Goal"], "confidence": 1.0},
        {"process": "PRODUCES", "kind": "relation", "args": [ALGO_A, RESOURCE_X], "confidence": 1.0},
        {"process": "REQUIRES", "kind": "relation", "args": [ALGO_B, RESOURCE_X], "confidence": 1.0},
        {"process": "PRODUCES", "kind": "relation", "args": [ALGO_B, GOAL], "confidence": 1.0},
    ]})


def has_direct_algorithm(ipc, goal) -> bool:
    resp = ipc.request("retrieve", {"query": goal})
    payload = resp.get("payload", {})
    if isinstance(payload, str):
        payload = json.loads(payload) if payload else {}

    for atom in payload.get("atoms", []):
        if atom.get("process") != "HAS_ALGORITHM":
            continue

        args = atom.get("args", [])

        if len(args) >= 2 and str(args[1]) == str(goal):
            return True
    return False


def activate(ipc):
    learn(ipc, {"atoms": [{"process": "IS_A", "kind": "relation", "args": [GOAL, "Goal"]}]})
    learn(ipc, {"nodes": [{"id": GOAL, "label": GOAL, "utility": 0.9}]})


def main():
    ipc = IPCClient()
    ipc.connect()
    assert ipc.ping(), "Core not responding"

    bootstrap_knowledge(ipc, force=False)
    register_building_blocks(ipc)
    assert not has_direct_algorithm(ipc, GOAL), "в базе уже есть прямой HAS_ALGORITHM для GOAL"
    print(f"[ZeroShot] Подтверждено: прямого HAS_ALGORITHM для '{GOAL}' нет.")

    activate(ipc)
    print("[ZeroShot] Цель активирована. Ждём ZeroShotComposer...")

    linked = False
    deadline = time.monotonic() + 15.0
    while time.monotonic() < deadline:
        ipc.command("think")
        time.sleep(0.35)
        if has_direct_algorithm(ipc, GOAL):
            linked = True
            print("[ZeroShot] Композитный алгоритм связан с целью.")
            break

        # Working Memory может затухнуть.
        # Периодически повторно активируем цель.
        if int((deadline - time.monotonic()) * 10) % 20 == 0:
            activate(ipc)

    assert linked, (f"ZeroShotComposer не синтезировал HAS_ALGORITHM(*, {GOAL})")
    print("[ZeroShot] УСПЕХ: ядро само скомпоновало новый алгоритм из A+B, используя "
          "только графовые знания PRODUCES/REQUIRES — без единой строчки Python-хардкода.")
    ipc.close()


if __name__ == "__main__":
    main()
