# app/core/sdk.py
"""
NeuroCore SDK — единый фасад Python-слоя над C-ядром KOSMOS.
"""
from __future__ import annotations
import asyncio
import json
import re
import threading
import time
from dataclasses import dataclass
from typing import Any, Optional, Union

from core.ipc import IPCClient, DEFAULT_SOCKET, DEFAULT_TIMEOUT

HYPER_VALUE_MASK = 0x3FFFFFFFFFFFFFFF

def djb2_hash(s: str) -> int:
    """Побитово совпадает с core/src/math/hash.c::djb2_hash()."""
    h = 5381
    for byte in s.encode("utf-8"):
        h = ((h << 5) + h + byte) & 0xFFFFFFFFFFFFFFFF
    return h & HYPER_VALUE_MASK

# ============================================================================
# JSON parsing
# ============================================================================
_MD_FENCE_OPEN = re.compile(r"^```(?:json)?", re.IGNORECASE)
_MD_FENCE_CLOSE = re.compile(r"```$")
_JSON_OBJ_OR_ARR = re.compile(r"[\{\[].*[\}\]]", re.DOTALL)

def parse_json(raw: Optional[str]) -> Optional[Union[dict, list]]:
    """
    Разбирает JSON, который вернула LLM, устойчиво к:
      - обрамлению в ```json ... ``` / ``` ... ```;
      - тексту до/после JSON-блока ("Вот твой JSON:\\n{...}\\nНадеюсь помог!");
      - ведущим/замыкающим пробелам.
    Возвращает None, если валидный JSON-объект/массив не найден —
    НИКОГДА не бросает исключение. Вызывающий код обязан уметь
    обработать случай "LLM не смогла вернуть валидный JSON".
    """
    if not raw:
        return None

    text = raw.strip()
    text = _MD_FENCE_OPEN.sub("", text).strip()
    text = _MD_FENCE_CLOSE.sub("", text).strip()

    try:
        return json.loads(text)
    except json.JSONDecodeError:
        pass

    match = _JSON_OBJ_OR_ARR.search(text)
    if match:
        try:
            return json.loads(match.group(0))
        except json.JSONDecodeError:
            return None

    return None

# ============================================================================
# Text chunking
# ============================================================================
DEFAULT_CHUNK_SIZE = 2800
DEFAULT_CHUNK_OVERLAP = 200
_SENTENCE_SPLIT = re.compile(r"(?<=[.!?])\s+")

def chunk_text(text: str, size: int = DEFAULT_CHUNK_SIZE, overlap: int = DEFAULT_CHUNK_OVERLAP) -> list[str]:
    sentences = _SENTENCE_SPLIT.split(text.strip())
    chunks: list[str] = []
    current = ""
    for s in sentences:
        if len(current) + len(s) + 1 > size and current:
            chunks.append(current.strip())
            current = current[-overlap:] + " " + s
        else:
            current = (current + " " + s).strip()
    if current.strip():
        chunks.append(current.strip())
    return chunks

# ============================================================================
# CoreClient — потокобезопасный фасад над IPC
# ============================================================================
DEFAULT_DOMAIN_ALGORITHM = 1

class CoreError(RuntimeError):
    pass

@dataclass
class EpisodeRecord:
    episode_id: int
    goal_id: int
    algorithm_id: int
    result_atom_id: int
    vm_status: int
    outcome: float
    duration_cycles: int
    wall_time: int

    @classmethod
    def from_payload(cls, d: dict) -> "EpisodeRecord":
        return cls(
            episode_id=int(d.get("episode_id", 0)),
            goal_id=int(d.get("goal_id", 0)),
            algorithm_id=int(d.get("algorithm_id", 0)),
            result_atom_id=int(d.get("result_atom_id", 0)),
            vm_status=int(d.get("vm_status", -1)),
            outcome=float(d.get("outcome", 0.0)),
            duration_cycles=int(d.get("duration_cycles", 0)),
            wall_time=int(d.get("wall_time", 0)),
        )

    @property
    def succeeded(self) -> bool:
        return self.vm_status == 0 and self.outcome >= 1.0

class CoreClient:
    def __init__(self, socket_path: str = DEFAULT_SOCKET, timeout: float = DEFAULT_TIMEOUT):
        self._ipc = IPCClient(socket_path=socket_path, timeout=timeout)
        self._lock = threading.Lock()

    def connect(self) -> "CoreClient":
        with self._lock:
            self._ipc.connect()
            if not self._ipc.ping():
                raise CoreError("NeuroCore core did not respond to ping()")
        return self

    def close(self) -> None:
        with self._lock:
            self._ipc.close()

    def __enter__(self) -> "CoreClient":
        return self.connect()

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()

    def _request(self, name: str, payload: Any = None) -> dict:
        with self._lock:
            return self._ipc.request(name, payload)

    def _command(self, name: str, payload: Any = None) -> dict:
        with self._lock:
            return self._ipc.command(name, payload)

    @staticmethod
    def _payload_of(resp: dict) -> dict:
        p = resp.get("payload", {})
        if isinstance(p, str):
            return json.loads(p) if p.strip() else {}
        return p or {}

    def learn(self, payload: dict) -> dict:
        resp = self._command("learn", json.dumps(payload))
        if resp.get("name") == "error":
            raise CoreError(f"learn() failed: {resp.get('payload')} (payload={payload})")
        return resp

    def learn_atoms(self, atoms: list[dict]) -> dict:
        return self.learn({"atoms": atoms})

    def learn_pipeline(self, algo_name: str, code: list[dict], constants: Optional[dict] = None) -> dict:
        return self.learn({
            "type": "pipeline",
            "algo_name": algo_name,
            "code": code,
            "constants": constants or {},
        })

    def link_algorithm(self, algo_name: str, goal_id: str, confidence: float = 1.0) -> dict:
        return self.learn({"atoms": [
            {"process": "IS_A", "kind": "relation",
             "args": ["HAS_ALGORITHM", "GoalAlgorithmRelation"], "confidence": 1.0},
            {"process": "HAS_ALGORITHM", "kind": "relation",
             "args": [algo_name, goal_id], "confidence": confidence},
        ]})

    def activate_goal(self, goal_id: str, utility: float = 0.9) -> None:
        self.learn({"atoms": [
            {"process": "IS_A", "kind": "relation", "args": [goal_id, "Goal"]}
        ]})

        self.learn({"nodes": [
            {"id": goal_id, "label": goal_id, "utility": utility}
        ]})

    def think(self) -> None:
        self._command("think")

    def clear_cooldown(self, goal_id: str) -> None:
        self._command("clear_cooldown", json.dumps({"goal": goal_id}))

    def get_score(self, subject: str, domain: int = DEFAULT_DOMAIN_ALGORITHM) -> float:
        resp = self._request("get_score", {"subject": subject, "domain": domain})
        return float(self._payload_of(resp).get("score", 0.5))

    def get_episodes(self, subject: str, limit: int = 20) -> list[EpisodeRecord]:
        resp = self._request("get_episodes", {"subject": subject, "limit": limit})
        payload = resp.get("payload", [])
        if isinstance(payload, str):
            payload = json.loads(payload) if payload.strip() else []
        return [EpisodeRecord.from_payload(e) for e in payload]

    def get_property(self, subject: str, key: str) -> dict:
        resp = self._request("get_property", {"subject": subject, "key": key})
        return self._payload_of(resp)

    def get_stats(self) -> dict:
        """Снимает метрики с LMDB таблиц ядра (количество знаний, эпизодов и т.д.)"""
        resp = self._request("get_stats")
        return self._payload_of(resp)

    def retrieve(self, query: str) -> dict:
        # Убрали .lower()! djb2_hash регистрозависим.
        resp = self._request("retrieve", {"query": query})
        return self._payload_of(resp)

    def exec_algorithm(self, algo_name: str, report_regs: list[int]) -> dict:
        payload = {
            "op": "exec_algorithm",
            "regs": {"5": algo_name},
            "report_regs": report_regs,
        }
        resp = self._command("execute_op", json.dumps(payload))
        return self._payload_of(resp).get("reported_regs", {})

    def wait_for_episode(self, goal_id: str, timeout_sec: float = 8.0, poll_interval: float = 0.25) -> Optional[EpisodeRecord]:
        deadline = time.monotonic() + timeout_sec
        while time.monotonic() < deadline:
            episodes = self.get_episodes(goal_id)
            if episodes:
                return episodes[0]
            time.sleep(poll_interval)
        return None

    # Async wrappers
    async def learn_async(self, payload: dict) -> dict:
        return await asyncio.to_thread(self.learn, payload)

    async def learn_atoms_async(self, atoms: list[dict]) -> dict:
        return await asyncio.to_thread(self.learn_atoms, atoms)

    async def activate_goal_async(self, goal_id: str, utility: float = 0.9) -> None:
        await asyncio.to_thread(self.activate_goal, goal_id, utility)

    async def think_async(self) -> None:
        await asyncio.to_thread(self.think)

    async def get_score_async(self, subject: str, domain: int = DEFAULT_DOMAIN_ALGORITHM) -> float:
        return await asyncio.to_thread(self.get_score, subject, domain)

    async def get_episodes_async(self, subject: str, limit: int = 20) -> list[EpisodeRecord]:
        return await asyncio.to_thread(self.get_episodes, subject, limit)
