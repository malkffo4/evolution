# app/knowledge/semantic_compiler.py
"""
Semantic Compiler: текст -> атомы HyperMemory по Neo-Davidsonian семантике.

Один факт = "созвездие" атомов:
  head-атом события:  id=event_id, process=EVENT_TYPE, args=[event_id, predicate]
  role-атомы:          id=event_id__role, process=ROLE, args=[event_id, entity_id], cause=event_id

Требует патч perception.c (perceive_hyper_json: id берётся из явного поля "id"
через djb2_hash) и патч cmd_learn (db_write_sync вызывается не только для pipeline).
"""

import json
import re
from dataclasses import dataclass, field

HYPER_VALUE_MASK = 0x3FFFFFFFFFFFFFFF


def djb2_hash(s: str) -> int:
    """Побитово совпадает с core/src/math/hash.c: djb2_hash()"""
    h = 5381
    for byte in s.encode("utf-8"):
        h = ((h << 5) + h + byte) & 0xFFFFFFFFFFFFFFFF
    return h & HYPER_VALUE_MASK


ALLOWED_ROLES = ("agent", "patient", "instrument", "location", "time", "manner")

AMR_EXTRACTION_PROMPT = """Ты — экстрактор событийной семантики (Neo-Davidsonian AMR).
Разбей текст на СОБЫТИЯ. Каждое событие — один предикат (действие/состояние/отношение)
и его семантические роли.

Правила:
- Не додумывай факты, которых нет в тексте.
- Каждой сущности дай стабильный id в snake_case (латиница), например "e_bloodhound".
- Каждому событию дай id вида "ev1", "ev2"... по порядку появления в тексте.
- predicate — ЗАГЛАВНЫМИ (USES, CAUSES, EXPLOITS, IS_A, HAS_PROPERTY, REQUIRES, LOCATED_IN...).
- roles: только явно присутствующие в тексте. Разрешённые ключи: agent, patient,
  instrument, location, time, manner. Значение — id сущности из "entities" либо null.
- confidence: 0..1, насколько дословно текст это утверждает (не обобщай).

Верни СТРОГО валидный JSON, без markdown и пояснений:
{{
  "entities": [{{"id": "e_bloodhound", "label": "BloodHound", "type": "Tool"}}],
  "events": [
    {{"id": "ev1", "predicate": "ANALYZES",
      "roles": {{"agent": "e_bloodhound", "patient": "e_active_directory"}},
      "confidence": 0.9}}
  ]
}}

Если фактов нет — верни {{"entities": [], "events": []}}.

Текст:
\"\"\"{chunk}\"\"\"
"""


def _repair_json(raw: str) -> dict | None:
    raw = raw.strip()
    raw = re.sub(r"^```(json)?", "", raw).strip()
    raw = re.sub(r"```$", "", raw).strip()
    try:
        return json.loads(raw)
    except json.JSONDecodeError:
        pass
    m = re.search(r"\{.*\}", raw, re.DOTALL)
    if m:
        try:
            return json.loads(m.group(0))
        except json.JSONDecodeError:
            return None
    return None


@dataclass
class CompileResult:
    atoms: list = field(default_factory=list)
    last_event_id: str | None = None  # для сцепки cause в следующем вызове (диалог)


class SemanticCompiler:
    def __init__(self, llm_client):
        self.llm = llm_client

    def _extract_frames(self, text: str) -> dict | None:
        raw = self.llm.query(
            AMR_EXTRACTION_PROMPT.format(chunk=text[:3000]),
            json_mode=True,
        )
        return _repair_json(raw) if raw else None

    def compile(self, text: str, context_id: str, prev_event_id: str | None = None) -> CompileResult:
        """
        context_id: строковый id контекста мышления/диалога
                    (например "chat_session_42" или "book:strcpy.pdf:chunk17").
        prev_event_id: id предыдущего события той же цепочки — для cause_id.
        """
        frames = self._extract_frames(text)
        result = CompileResult()
        if not frames:
            return result

        entities = {e["id"]: e for e in frames.get("entities", []) if e.get("id")}
        event_ids = {ev["id"] for ev in frames.get("events", []) if ev.get("id")}

        for eid, ent in entities.items():
            result.atoms.append({
                "id": eid, "process": "IS_A",
                "args": [eid, ent.get("type", "Entity")],
                "context": context_id, "confidence": 1.0,
            })
            if ent.get("label"):
                result.atoms.append({
                    "id": f"{eid}__label", "process": "HAS_LABEL",
                    "args": [eid, ent["label"]], "context": context_id,
                })

        chain_prev = prev_event_id
        for ev in frames.get("events", []):
            eid, predicate = ev.get("id"), ev.get("predicate")
            if not eid or not predicate:
                continue
            conf = float(ev.get("confidence", 0.6))

            result.atoms.append({
                "id": eid, "process": "EVENT_TYPE",
                "args": [eid, predicate],
                "context": context_id, "cause": chain_prev, "confidence": conf,
            })

            for role_name, target_id in (ev.get("roles") or {}).items():
                if role_name not in ALLOWED_ROLES or not target_id:
                    continue
                if target_id not in entities and target_id not in event_ids:
                    continue  # ссылка на несуществующую сущность — не выдумываем
                result.atoms.append({
                    "id": f"{eid}__{role_name}", "process": role_name.upper(),
                    "args": [eid, target_id],
                    "context": context_id, "cause": eid, "confidence": conf,
                })

            chain_prev = eid

        result.last_event_id = chain_prev
        return result
