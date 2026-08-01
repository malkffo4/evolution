#!/usr/bin/env python3
# app/tools/interact.py
"""
Неблокирующая активация цели и асинхронное ожидание результата опросом IPC.

Путь данных:
  interact.py --activate_goal()->learn (nodes+IS_A)  [IPC, db_write_sync,
      короткая транзакция активации WM, доли миллисекунды]
        -> global_wm, узел активирован
        -> dmn_loop() просыпается сам (bekoff до 3.2с) или по nudge()="think"
        -> MainLoop (Pipeline в LMDB) -> OP_EVALUATE_GOALS -> CorePlanner
             OP_WM_TOP_GOAL -> OP_SELECT_ALGORITHM (UCB1) -> OP_DISPATCH_ASYNC
        -> vm_pool: новый поток, своя HyperMemory/WorkingMemory, db_write_sync
             Episode + Score(ALGORITHM) + credit propagation
        -> interact.py::wait_for_episode() опрашивает get_episodes()

Работает с уже КЛАССИФИЦИРОВАННЫМИ целями — теми, для кого в базе есть
хотя бы один атом HAS_ALGORITHM (core/bootstrap.py, app/tools/book_loader.py).
Перевод произвольного текста в такую цель — задача подсистемы Understanding
(app/services/chat_service.py, app/knowledge/semantic_compiler.py); этот
скрипт демонстрирует нижний, символьный слой, в который в итоге упирается
любой верхний NLU-фронтенд.
"""
import argparse
import json
import sys
import time
from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[1]
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

from core.ipc import IPCClient


def _learn(ipc: IPCClient, payload: dict) -> dict:
    resp = ipc.command("learn", json.dumps(payload))
    if resp.get("name") == "error":
        raise RuntimeError(f"learn failed: {resp.get('payload')} (payload={payload})")
    return resp


def _as_payload(resp: dict):
    payload = resp.get("payload", {})
    if isinstance(payload, str):
        payload = json.loads(payload) if payload else {}
    return payload


def activate_goal(ipc: IPCClient, goal_name: str, utility: float = 0.9) -> None:
    """Неблокирующая постановка цели: cmd_learn пишет факт и активацию
    через db_write_sync — короткая write-транзакция на один узел, не
    решение задачи. Сама когниция происходит асинхронно."""
    _learn(ipc, {"atoms": [
        {"process": "IS_A", "kind": "relation", "args": [goal_name, "Goal"], "confidence": 1.0}
    ]})
    _learn(ipc, {"nodes": [
        {"id": goal_name, "label": goal_name, "danger": 0.1, "utility": utility}
    ]})


def nudge(ipc: IPCClient) -> None:
    """cmd_think выставляет g_think_trigger=1 и возвращается немедленно —
    будит dmn_loop без ожидания экспоненциального бэкоффа простоя."""
    ipc.command("think")


def get_score(ipc: IPCClient, subject: str, domain: int = 1) -> float:
    resp = ipc.request("get_score", {"subject": subject, "domain": domain})
    return float(_as_payload(resp).get("score", 0.5))


def get_episodes(ipc: IPCClient, subject: str, limit: int = 10) -> list:
    resp = ipc.request("get_episodes", {"subject": subject, "limit": limit})
    payload = resp.get("payload", [])
    if isinstance(payload, str):
        payload = json.loads(payload) if payload else []
    return payload


def wait_for_episode(ipc: IPCClient, goal_name: str, timeout_sec: float,
                      poll_interval: float = 0.25):
    """Блокируется только клиент, циклом опроса — ядро всё это время
    свободно и обслуживает другие IPC-запросы. Настоящий push (IPC EVENT
    -> подписчик, см. docs/12_IPC.md Publish/Subscribe) — следующий шаг;
    сегодня опрос честнее, потому что переживает разрыв UDS-сессии."""
    deadline = time.monotonic() + timeout_sec
    seen = set()
    while time.monotonic() < deadline:
        for ep in get_episodes(ipc, goal_name):
            if ep["episode_id"] not in seen:
                seen.add(ep["episode_id"])
                return ep
        time.sleep(poll_interval)
    return None


def main():
    ap = argparse.ArgumentParser(description="NeuroCore: async goal activation demo")
    ap.add_argument("goal", help="Имя цели, уже связанной HAS_ALGORITHM "
                                  "(например ComputeAverage после book_loader.py)")
    ap.add_argument("--algorithm", default=None,
                     help="Если известно имя алгоритма — показать его Score после эпизода")
    ap.add_argument("--utility", type=float, default=0.9)
    ap.add_argument("--timeout", type=float, default=8.0)
    args = ap.parse_args()

    ipc = IPCClient()
    ipc.connect()
    assert ipc.ping(), "Core not responding"

    print(f"[interact] activating goal '{args.goal}' (non-blocking)...")
    t0 = time.monotonic()
    activate_goal(ipc, args.goal, args.utility)
    nudge(ipc)
    print(f"[interact] IPC-вызов вернул управление за {time.monotonic()-t0:.4f}s — "
          f"дальше когниция идёт в фоновом actor pool.")

    ep = wait_for_episode(ipc, args.goal, timeout_sec=args.timeout)
    if ep is None:
        print(f"[interact] эпизод не появился за {args.timeout}s. Возможные причины:\n"
              f"  - для '{args.goal}' нет ни одного HAS_ALGORITHM -> "
              f"wm_get_highest_goal() вообще не считает узел целью;\n"
              f"  - HAS_ALGORITHM есть, но кандидатов ноль -> CorePlanner уже "
              f"поставил '{args.goal}' в очередь research_worker.py и на cooldown;\n"
              f"  - MainLoop ещё не дошёл до тика (маловероятно после nudge()).")
    else:
        dt = time.monotonic() - t0
        print(f"[interact] эпизод записан: id={ep['episode_id']} "
              f"algorithm_id={ep['algorithm_id']} vm_status={ep['vm_status']} "
              f"outcome={ep['outcome']}  (end-to-end {dt:.3f}s)")

    if args.algorithm:
        print(f"[interact] Score('{args.algorithm}', ALGORITHM) = "
              f"{get_score(ipc, args.algorithm):.4f}")

    ipc.close()


if __name__ == "__main__":
    main()