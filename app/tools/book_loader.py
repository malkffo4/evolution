#!/usr/bin/env python3
# app/tools/book_loader.py
"""
Минимальный, честный загрузчик "книги алгоритмов" в HyperMemory.

ЭТО НЕ Knowledge Extraction (NLP) из TODO Priority 3 / RFC-0002.
Полноценное text -> NER -> relations -> HyperAtoms остаётся будущей задачей.

Что делает загрузка одного algorithm-entry:
  1. IS_A(name, "Algorithm")               [kind=relation]
  2. IS_A(solves_goal, "Goal")              [kind=relation]
  3. HAS_ALGORITHM(name, solves_goal)       [kind=relation]
  4. HAS_LABEL(name, description)           [kind=entity]
  5. Регистрация ИСПОЛНЯЕМОГО пайплайна через уже существующий
     pipeline_from_json() / cmd_learn(type="pipeline") — содержимое
     книги сразу становится исполняемым VM bytecode.

Всё идёт через существующий IPC "learn" — ни одной новой C-функции для
самой загрузки не требуется.
"""
import json
import sys
from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[1]
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

from core.ipc import IPCClient

_META_ATOMS = [
    {"process": "IS_A", "kind": "relation",
     "args": ["HAS_ALGORITHM", "GoalAlgorithmRelation"], "confidence": 1.0},
]


def _learn(ipc, payload: dict) -> dict:
    resp = ipc.command("learn", json.dumps(payload))
    if resp.get("name") == "error":
        raise RuntimeError(f"learn failed: {resp.get('payload')} (payload: {payload})")
    return resp


def ensure_meta(ipc):
    _learn(ipc, {"atoms": _META_ATOMS})


def load_book(ipc, book_path: Path, verbose: bool = True) -> list:
    book = json.loads(book_path.read_text(encoding="utf-8"))
    ensure_meta(ipc)

    loaded = []
    for algo in book.get("algorithms", []):
        name = algo["name"]
        goal = algo["solves_goal"]
        description = algo.get("description", "")

        facts = [
            {"process": "IS_A", "kind": "relation", "args": [name, "Algorithm"], "confidence": 1.0},
            {"process": "IS_A", "kind": "relation", "args": [goal, "Goal"], "confidence": 1.0},
            {"process": "HAS_ALGORITHM", "kind": "relation", "args": [name, goal], "confidence": 1.0},
        ]
        if description:
            facts.append({"process": "HAS_LABEL", "kind": "entity",
                          "args": [name, description], "confidence": 1.0})
        _learn(ipc, {"atoms": facts})

        pipeline_payload = {
            "type": "pipeline",
            "algo_name": name,
            "code": algo["code"],
            "constants": algo.get("constants", {}),
        }
        _learn(ipc, pipeline_payload)

        loaded.append(name)
        if verbose:
            print(f"[book_loader] Loaded '{name}' (solves '{goal}'): {description}")

    if verbose:
        print(f"[book_loader] Book '{book.get('title', book_path.name)}': "
              f"{len(loaded)} algorithm(s) -> HyperMemory + executable pipelines.")
    return loaded


def main():
    if len(sys.argv) < 2:
        print("Usage: book_loader.py <path/to/book.json>")
        sys.exit(1)
    ipc = IPCClient()
    ipc.connect()
    assert ipc.ping(), "Core not responding"
    load_book(ipc, Path(sys.argv[1]))
    ipc.close()


if __name__ == "__main__":
    main()
