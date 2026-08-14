# app/knowledge/sensors/base.py
from __future__ import annotations
import uuid
from abc import ABC, abstractmethod
from typing import Iterable

from core.sdk import djb2_hash
from knowledge.domain_ns import namespace_entity
from knowledge.embeddings import embed_text

QUEUE_NAME = "cybersec::log_classify"


class SensorAdapter(ABC):
    """Переводит вывод конкретного инструмента в нейтральный формат.
    Адаптер НЕ знает, что "открытый порт 80" значит потенциальную атаку —
    это знание должно быть выучено (текст+эмбеддинги), а не зашито тут."""
    tool_name: str = "unknown_tool"

    @abstractmethod
    def parse(self, raw_output: str) -> Iterable[dict]:
        """{"entity": str, "properties": {...}, "text": str для эмбеддинга}"""
        raise NotImplementedError

    def to_atoms(self, raw_output: str, scope: str, session_tag: str | None = None) -> list[dict]:
        session_tag = session_tag or f"{self.tool_name}_{uuid.uuid4().hex[:8]}"
        scope_ns = namespace_entity(scope, "cybersec")
        scope_ctx = djb2_hash(scope_ns)   # perception.c читает "context" ТОЛЬКО как число!
        atoms = []

        for finding in self.parse(raw_output):
            entity = namespace_entity(finding["entity"], "cybersec")
            text_for_embedding = finding.get("text", finding["entity"])

            atoms.append({
                "id": f"{session_tag}_{uuid.uuid4().hex[:8]}",
                "process": "OBSERVED_EVENT",
                "kind": "event",
                "args": [entity, scope_ns],
                "truth": {"mean": 1.0, "confidence": 1.0},   # инструмент не врёт о том, что увидел
                "attention": {"sti": 0.6, "lti": 0.08},        # намеренно недолговечно — см. archive_gc
                "context": scope_ctx,
                "properties": {
                    "tool": self.tool_name, "session": session_tag,
                    **{k: v for k, v in finding.get("properties", {}).items()
                       if isinstance(v, (str, int, float, bool))},
                },
                "embedding": embed_text(text_for_embedding),
                "enqueue": QUEUE_NAME,
            })
        return atoms
