#!/usr/bin/env python3
# app/core/sdk.py
"""
NeuroCore SDK — единый фасад Python-слоя над C-ядром KOSMOS.

Зачем этот модуль
------------------
До рефакторинга по всему репозиторию (app/tools/*.py, app/services/*.py,
app/agent.py) было независимо продублировано:

  - parse_json() / _parse_json()   — mvp_agent.py, semantic_compiler.py,
                                      agent.py, ingest_knowledge.py,
                                      deep_extractor.py, knowledge_validator.py
  - chunk_text()                   — ingest_knowledge.py, deep_extractor.py
                                      (две почти идентичные копии)
  - learn()/_learn()               — agi_client.py, interact.py,
                                      book_loader.py, agent.py,
                                      learning_demo_arithmetic.py, ...
  - activate_goal()                — agi_client.py, interact.py, agent.py
  - get_score()/get_episodes()     — тот же паттерн в 5+ местах
  - think()                        — тот же паттерн в 4+ местах

Всё это стянуто сюда, за единым интерфейсом CoreClient. Старые модули
(например app/tools/agi_client.py) теперь — тонкие совместимые обёртки
поверх CoreClient, чтобы не переписывать вызывающий код по всему репо.

Потокобезопасность
-------------------
core.ipc.IPCClient держит ОДИН сокет с half-duplex протоколом
(заголовок -> payload -> flags, синхронный request/response). Ядро
физически поддерживает много одновременных клиентов (transport.c:
MAX_CLIENTS, отдельный поток на клиента), но конкретный Python-объект
IPCClient — нет: если два потока одновременно вызовут send()/recv() на
одном и том же сокете, их байты перемешаются на проводе, и оба получат
либо чужой ответ, либо зависнут навсегда.

CoreClient держит один threading.Lock на инстанс и берёт его на время
КАЖДОГО request()/command(). Это безопасно как для ThreadPoolExecutor-
воркеров, так и для asyncio.to_thread() — в обоих случаях реальный вызов
происходит в потоке ОС, а threading.Lock корректно сериализует доступ
независимо от того, кто его дёргает (поток или корутина).

Если нужен параллелизм именно по IPC (не только по LLM), поднимите
несколько независимых CoreClient (каждый — свой сокет; ядро это
позволяет), а не несколько потоков на одном CoreClient.
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


# ============================================================================
# JSON parsing — было продублировано в 6+ файлах
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
# Text chunking — было продублировано в ingest_knowledge.py и deep_extractor.py
# ============================================================================

DEFAULT_CHUNK_SIZE = 2800     # запас под промпты вида f"...{chunk[:3000]}..."
DEFAULT_CHUNK_OVERLAP = 200   # не рвём сущность/предложение на границе чанка

_SENTENCE_SPLIT = re.compile(r"(?<=[.!?])\s+")


def chunk_text(
    text: str, size: int = DEFAULT_CHUNK_SIZE, overlap: int = DEFAULT_CHUNK_OVERLAP
) -> list[str]:
    """Режет текст по границам предложений (не разрывая слова/сущности),
    с overlap для сохранения контекста на стыке соседних чанков."""
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

DEFAULT_DOMAIN_ALGORITHM = 1  # COGNITIVE_DOMAIN_ALGORITHM, knowledge/evaluation.h


class CoreError(RuntimeError):
    """learn()/etc. вернули {"error": ...}, либо ядро недоступно."""


@dataclass
class EpisodeRecord:
    """Типизированная проекция ответа get_episodes() вместо голых dict."""

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

    def as_dict(self) -> dict:
        return {
            "episode_id": self.episode_id,
            "goal_id": self.goal_id,
            "algorithm_id": self.algorithm_id,
            "result_atom_id": self.result_atom_id,
            "vm_status": self.vm_status,
            "outcome": self.outcome,
            "duration_cycles": self.duration_cycles,
            "wall_time": self.wall_time,
        }


class CoreClient:
    """
    Единая точка входа Python-слоя в C-ядро NeuroCore.

    Синхронное использование (из любого потока):

        core = CoreClient().connect()
        core.learn({"atoms": [...]})
        core.activate_goal("ComputeAverage")
        core.think()
        score = core.get_score("AverageOfThree")

    Использование из asyncio (параллельные LLM-вызовы + сериализованная
    запись в ядро):

        async def worker(core: CoreClient, chunk: str):
            atoms = await llm.aquery(...)              # параллельно, свой httpx
            await core.learn_async({"atoms": atoms})   # сериализовано локом

    Инстанс CoreClient можно безопасно передавать одновременно и в
    ThreadPoolExecutor, и в набор asyncio-корутин — см. docstring модуля.
    """

    def __init__(self, socket_path: str = DEFAULT_SOCKET, timeout: float = DEFAULT_TIMEOUT):
        self._ipc = IPCClient(socket_path=socket_path, timeout=timeout)
        self._lock = threading.Lock()

    # ---- lifecycle ---------------------------------------------------

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

    # ---- raw IPC, locked -----------------------------------------------

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

    # ---- knowledge write -------------------------------------------------

    def learn(self, payload: dict) -> dict:
        """POST атомов/nodes/pipeline в LMDB. Бросает CoreError при отказе."""
        resp = self._command("learn", json.dumps(payload))
        if resp.get("name") == "error":
            raise CoreError(f"learn() failed: {resp.get('payload')} (payload={payload})")
        return resp

    def learn_atoms(self, atoms: list[dict]) -> dict:
        """Короткий путь для самого частого случая: {"atoms": [...]}."""
        return self.learn({"atoms": atoms})

    def learn_pipeline(self, algo_name: str, code: list[dict], constants: Optional[dict] = None) -> dict:
        """
        Регистрирует исполняемый Pipeline (Instruction[] как JSON) — тот
        же формат, что knowledge/pipeline_io.c::pipeline_from_json()
        ожидает на входе (см. book_loader.py / bootstrap.py / agent.py,
        где этот словарь раньше собирался вручную в каждом файле).
        """
        return self.learn({
            "type": "pipeline",
            "algo_name": algo_name,
            "code": code,
            "constants": constants or {},
        })

    def link_algorithm(self, algo_name: str, goal_id: str, confidence: float = 1.0) -> dict:
        """
        HAS_ALGORITHM(algo_name, goal_id) + обязательный мета-факт
        IS_A(HAS_ALGORITHM, GoalAlgorithmRelation) — без него
        find_goal_algorithm_relations() (reasoning/algorithm_planner.c)
        не увидит связь между целью и алгоритмом.
        """
        return self.learn({"atoms": [
            {"process": "IS_A", "kind": "relation",
             "args": ["HAS_ALGORITHM", "GoalAlgorithmRelation"], "confidence": 1.0},
            {"process": "HAS_ALGORITHM", "kind": "relation",
             "args": [algo_name, goal_id], "confidence": confidence},
        ]})

    def activate_goal(self, goal_id: str, utility: float = 0.9) -> None:
        """
        Регистрирует IS_A(goal_id, Goal) и активирует узел в Working
        Memory (docs/10_VM.md: Virtual Mind -> Working Memory ->
        CorePlanner). Неблокирующе с точки зрения когниции: сама задача
        решается асинхронно MainLoop-демоном (memory/subconscious.c).
        """
        self.learn({"atoms": [
            {"process": "IS_A", "kind": "relation", "args": [goal_id, "Goal"], "confidence": 1.0}
        ]})
        self.learn({"nodes": [
            {"id": goal_id, "label": goal_id, "danger": 0.1, "utility": utility}
        ]})

    def think(self) -> None:
        """Будит dmn_loop немедленно (g_think_trigger=1), не дожидаясь backoff."""
        self._command("think")

    def clear_cooldown(self, goal_id: str) -> None:
        self._command("clear_cooldown", json.dumps({"goal": goal_id}))

    # ---- knowledge read --------------------------------------------------

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

    def retrieve(self, query: str) -> dict:
        resp = self._request("retrieve", {"query": query.lower()})
        return self._payload_of(resp)

    def exec_algorithm(self, algo_name: str, report_regs: list[int]) -> dict:
        """
        Синхронный прямой запуск уже скомпилированного алгоритма
        (execute_op), в обход Goal -> Planner -> vm_pool. Возвращает
        {"<reg_idx_str>": value, ...} для запрошенных регистров.
        """
        payload = {
            "op": "exec_algorithm",
            "regs": {"5": algo_name},
            "report_regs": report_regs,
        }
        resp = self._command("execute_op", json.dumps(payload))
        return self._payload_of(resp).get("reported_regs", {})

    # ---- higher-level: poll for async cognition result --------------------

    def wait_for_episode(
        self, goal_id: str, timeout_sec: float = 8.0, poll_interval: float = 0.25
    ) -> Optional[EpisodeRecord]:
        """
        Cognitive Cycle асинхронен (RFC-0001): activate_goal()+think()
        возвращают управление немедленно, реальное исполнение идёт в
        vm_pool-воркере. Этот метод — единственный БЛОКИРУЮЩИЙ способ
        дождаться результата; сам опрос не держит ядро занятым.
        """
        deadline = time.monotonic() + timeout_sec
        while time.monotonic() < deadline:
            episodes = self.get_episodes(goal_id)
            if episodes:
                return episodes[0]
            time.sleep(poll_interval)
        return None

    # ---- async wrappers (для asyncio-конвейеров, напр. ingest_knowledge.py) --
    #
    # Каждый вызов уходит в default ThreadPoolExecutor через to_thread():
    # сам IPC-вызов синхронный и короткий (db_write_sync на короткую
    # write-транзакцию, см. cmd_execute.c/cmd.c), поэтому блокировка потока
    # на десятки-сотни микросекунд не создаёт узкого места даже при
    # высокой конкурентности LLM-задач. Threading.Lock внутри _request()/
    # _command() сериализует фактический доступ к сокету.

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


# ============================================================================
# Module-level convenience — для скриптов, которым не нужен целый класс
# ============================================================================

def connect(socket_path: str = DEFAULT_SOCKET, timeout: float = DEFAULT_TIMEOUT) -> CoreClient:
    """core = sdk.connect() — самый частый способ входа в app/tools/*.py."""
    return CoreClient(socket_path=socket_path, timeout=timeout).connect()
