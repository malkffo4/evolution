# app/knowledge/log_classifier.py
"""
Уровень 4 AGI Olympics (docs/Evaluation & AGI Olympics.md): перенос
усвоенной онтологии на данные, которых не было в обучающем тексте.

Лог -> LLM извлекает каноническую технику -> семантическое связывание
с уже известной техникой (embed_text + find_similar — тот же фоллбэк,
что knowledge/retrieval.py применяет для остального графа) ->
hyper_find_by_participant(technique_id) ищет HAS_TECHNIQUE(stage, technique)
-> Hypothesis-атом с confidence < 0.5.

Важно: это НАМЕРЕННО не Fact и не HAS_ALGORITHM. Гипотеза не должна
запускать никакого исполняемого поведения — только текстовую рекомендацию
человеку. См. req_advise.c для второго рубежа защиты на выдаче.
"""
import re
import time
import uuid

from knowledge.embeddings import embed_text
from knowledge.deep_extractor import _parse_json

LOG_EVENT_PROMPT = """Ты — экстрактор индикаторов из технического лога.
Извлеки ОДНУ каноническую технику поведения, максимально близко к
терминологии Cyber Kill Chain, в snake_case на английском. Не указывай
стадию — это отдельная задача классификации, не додумывай её здесь.

Верни СТРОГО JSON: {{"technique": "dns_beaconing"}}

Лог:
\"\"\"{log_line}\"\"\"
"""

STAGE_HYPOTHESIS_PROCESS = "STAGE_HYPOTHESIS"
HYPOTHESIS_CONFIDENCE = 0.35   # осознанно ниже 0.5 — это гипотеза, не факт


def _extract_technique(llm, log_line: str) -> str:
    raw = llm.query(LOG_EVENT_PROMPT.format(log_line=log_line[:1000]), json_mode=True)
    data = _parse_json(raw) or {}
    tech = str(data.get("technique", "")).strip().lower()
    return re.sub(r"[^a-z0-9_]+", "_", tech) or "unknown_technique"


def _resolve_technique(core, technique_label: str) -> str:
    """Точное совпадение сначала (дёшево); иначе ближайший семантический
    сосед среди уже известных сущностей. Не находит соседа — возвращает
    как есть: классификатор ниже честно откажется строить гипотезу вместо
    того, чтобы связать с случайной стадией."""
    if core.retrieve(technique_label).get("atoms"):
        return technique_label

    neighbors = core.find_similar(embed_text(technique_label), top_k=3)
    for n in neighbors:
        if n.get("label"):
            return n["label"]
    return technique_label


def ingest_log_line(core, log_line: str, llm, session_tag: str = "adhoc") -> dict:
    technique_raw = _extract_technique(llm, log_line)
    technique_id = _resolve_technique(core, technique_raw)
    event_tag = f"LogEvent_{session_tag}_{uuid.uuid4().hex[:8]}"

    core.learn({"atoms": [{
        "id": event_tag,
        "process": "OBSERVED_EVENT",
        "kind": "event",
        "args": [event_tag, technique_id],
        "truth": {"mean": 1.0, "confidence": 1.0},   # само наблюдение бесспорно
        "attention": {"sti": 0.6, "lti": 0.1},
        "properties": {"raw_log": log_line[:500], "session": session_tag},
    }]})

    stage = None
    for a in core.retrieve(technique_id).get("atoms", []):
        if a.get("process") == "HAS_TECHNIQUE" and technique_id in a.get("args", []):
            stage = a["args"][0]
            break

    if stage is not None:
        core.learn({"atoms": [{
            "process": STAGE_HYPOTHESIS_PROCESS,
            "kind": "hypothesis",
            "args": [event_tag, stage],
            "truth": {"mean": HYPOTHESIS_CONFIDENCE, "confidence": 0.6},
            "attention": {"sti": 0.3, "lti": 0.05},
            "cause": event_tag,
        }]})

    return {"event_id": event_tag, "technique": technique_id, "classified": stage is not None, "stage": stage}


def wait_for_hypothesis(core, event_id: str, timeout_sec: float = 8.0, poll_interval: float = 0.4) -> dict | None:
    """ingest_log_line пишет гипотезу синхронно — обычно находится с
    первой попытки. Опрос сохранён ради совместимости с будущим
    асинхронным путём (Goal -> MainLoop -> материализованный
    LogClassifier Pipeline)."""
    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        for a in core.retrieve(event_id).get("atoms", []):
            if a.get("process") == STAGE_HYPOTHESIS_PROCESS and len(a.get("args", [])) >= 2:
                return {"stage": a["args"][1],
                        "confidence": a.get("truth_confidence", HYPOTHESIS_CONFIDENCE),
                        "process": a["process"]}
        time.sleep(poll_interval)
    return None
