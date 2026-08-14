# app/knowledge/sensors/json_finding_adapter.py
import json
from .base import SensorAdapter

class JSONFindingAdapter(SensorAdapter):
    """Общий адаптер для JSON-lines (nuclei -jsonl, httpx -json...).
    Не парсит семантику тега/шаблона — просто переносит поля."""

    def __init__(self, entity_field: str, text_fields: list[str], tool_name: str = "json_tool"):
        self.entity_field, self.text_fields, self.tool_name = entity_field, text_fields, tool_name

    def parse(self, raw_output: str):
        for line in raw_output.splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                continue
            entity = str(rec.get(self.entity_field, "")).strip()
            if not entity:
                continue
            text = " ".join(str(rec.get(f, "")) for f in self.text_fields if rec.get(f))
            props = {k: v for k, v in rec.items() if isinstance(v, (str, int, float, bool))}
            yield {"entity": entity, "text": text or entity, "properties": props}
