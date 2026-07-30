#!/usr/bin/env python3
# app/tools/agi_client.py
"""Тонкая обёртка над core.ipc.IPCClient и IPC-командами learn/think/
get_score/get_episodes/execute_op. Не новая подсистема."""
import json
import sys
import time
from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[1]
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

from core.ipc import IPCClient

DOMAIN_ALGORITHM = 1  # COGNITIVE_DOMAIN_ALGORITHM, knowledge/evaluation.h


def connect() -> IPCClient:
    ipc = IPCClient()
    ipc.connect()
    assert ipc.ping(), "Core not responding"
    return ipc


def learn(ipc: IPCClient, payload: dict) -> dict:
    resp = ipc.command("learn", json.dumps(payload))
    if resp.get("name") == "error":
        raise RuntimeError(f"learn failed: {resp.get('payload')} (payload: {payload})")
    return resp


def activate_goal(ipc: IPCClient, goal_id: str, utility: float = 0.9):
    """activation/usefulness >= 0.6/0.7, порог wm_get_highest_goal()."""
    learn(ipc, {"nodes": [{"id": goal_id, "label": goal_id, "danger": 0.1, "utility": utility}]})


def think(ipc: IPCClient, settle_sec: float = 0.3):
    ipc.command("think")
    time.sleep(settle_sec)


def _as_dict(resp: dict) -> dict:
    payload = resp.get("payload", {})
    if isinstance(payload, str):
        payload = json.loads(payload) if payload else {}
    return payload


def get_score(ipc: IPCClient, subject: str, domain: int = DOMAIN_ALGORITHM) -> float:
    resp = ipc.request("get_score", {"subject": subject, "domain": domain})
    return float(_as_dict(resp).get("score", 0.5))


def get_episodes(ipc: IPCClient, subject: str, limit: int = 20) -> list:
    resp = ipc.request("get_episodes", {"subject": subject, "limit": limit})
    payload = resp.get("payload", [])
    if isinstance(payload, str):
        payload = json.loads(payload) if payload else []
    return payload


def exec_algorithm_and_read(ipc: IPCClient, algo_name: str, report_regs: list) -> dict:
    """Выполняет алгоритм напрямую (op=exec_algorithm) и возвращает
    значения запрошенных регистров — числовой результат вычисления,
    а не только успех/неудача."""
    payload = {
        "op": "exec_algorithm",
        "args": [5],
        "regs": {"5": algo_name},   # хэшируется на сервере (см. фикс cmd_execute.c)
        "report_regs": report_regs,
    }
    resp = ipc.request("execute_op", payload)
    return _as_dict(resp).get("reported_regs", {})
