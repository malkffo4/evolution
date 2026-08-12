# app/knowledge/deep_extractor.py
"""
Deep Knowledge Ingestion (Шаг 1): LLM как парсер ОТКРЫТОЙ онтологии.

В отличие от knowledge/prompts.py::EXTRACTION_PROMPT (только базовые
когнитивные векторы), здесь LLM ДОПОЛНИТЕЛЬНО прикрепляет к атому
произвольный словарь properties{} — любые предметно-специфичные
атрибуты, какие реально есть в тексте. Схемы у properties нет.

Физика C-ядра не меняется:
  - truth/attention/utility/valence -> 64-байтный NeuroAtom
  - properties{}                    -> db.graph.properties
Обработка: core/src/perception/perception.c::perceive_hyper_json (патч выше).
"""
import json
import re
import sys
import time
from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[1]
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

from knowledge.domain_ns import namespace_atom_args, DEFAULT_DOMAIN
from knowledge.embeddings import augment_atoms_with_entity_embeddings

DEEP_EXTRACTION_PROMPT = """Ты — когнитивный экстрактор знаний NeuroCore.
Читай ТОЛЬКО то, что явно написано или логически прямо следует из текста.

Для каждого факта:
1. Выдели триплет (process, args).
2. Оцени ОБЯЗАТЕЛЬНЫЕ когнитивные векторы truth/attention/utility/valence
   (без них Memory отклонит атом на этапе Validation).
3. ДОПОЛНИТЕЛЬНО, если текст даёт больше информации об этой сущности/связи,
   чем укладывается в триплет — прикрепи её в properties{}. Здесь НЕТ
   фиксированной схемы: любые предметные атрибуты (единицы измерения,
   версии, даты, CVE, координаты, химическая формула, юридический статус —
   что угодно, что реально есть в тексте). Не придумывай поля, которых
   нет в тексте. Значения: числа, строки или bool. Не вкладывай объекты
   глубже одного уровня.

Верни СТРОГО валидный JSON, без пояснений и markdown:
{{
  "atoms": [
    {{
      "process": "USES|CAUSES|HAS_PROPERTY|...",
      "kind": "relation",
      "args": ["subject_id", "object_id"],
      "truth": {{"mean": 0.9, "confidence": 0.7}},
      "attention": {{"sti": 0.6, "lti": 0.3}},
      "utility": 0.5,
      "valence": 0.0,
      "properties": {{
        "cve_id": "CVE-2024-1234",
        "cvss_score": 9.8,
        "patched": false
      }}
    }}
  ]
}}

Если фактов нет — верни {{"atoms": []}}.

Текст:
\"\"\"{chunk}\"\"\"
"""


def _parse_json(raw: str):
    if not raw:
        return None
    raw = re.sub(r"^```(json)?", "", raw.strip()).strip()
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


def chunk_text(text: str, size: int = 2800, overlap: int = 200) -> list:
    sentences = re.split(r"(?<=[.!?])\s+", text.strip())
    chunks, current = [], ""
    for s in sentences:
        if len(current) + len(s) + 1 > size and current:
            chunks.append(current.strip())
            current = current[-overlap:] + " " + s
        else:
            current = (current + " " + s).strip()
    if current.strip():
        chunks.append(current.strip())
    return chunks


def extract_deep(llm, chunk: str) -> list:
    raw = llm.query(DEEP_EXTRACTION_PROMPT.format(chunk=chunk[:3000]), json_mode=True)
    data = _parse_json(raw) or {}
    return data.get("atoms", [])


def ingest_text(ipc, llm, text: str, source_tag: str = "unknown", verbose: bool = True, domain: str = DEFAULT_DOMAIN) -> dict:
    """Текст -> LLM -> Навешивание Namespace и Эмбеддингов -> IPC "learn" -> LMDB."""
    chunks = chunk_text(text)
    total_atoms = 0
    if verbose:
        print(f"[deep_extractor] '{source_tag}' (domain: {domain}): {len(chunks)} chunk(s)")

    for i, chunk in enumerate(chunks, 1):
        atoms = extract_deep(llm, chunk)
        if not atoms:
            continue

        # 1. Применяем namespace к аргументам фактов
        for a in atoms:
            a.setdefault("context", source_tag)
            namespace_atom_args(a, domain)

        # 2. Генерируем атомы эмбеддингов для новых сущностей
        atoms = augment_atoms_with_entity_embeddings(atoms)

        resp = ipc.command("learn", json.dumps({"atoms": atoms}))
        if resp.get("name") == "error":
            if verbose:
                print(f"  [{i}/{len(chunks)}] learn failed: {resp.get('payload')}", file=sys.stderr)
            continue

        total_atoms += len(atoms)
        n_props = sum(len(a.get("properties", {})) for a in atoms)
        if verbose:
            print(f"  [{i}/{len(chunks)}] +{len(atoms)} atoms (+{n_props} properties)")
        time.sleep(0.05)

    return {"source": source_tag, "chunks": len(chunks), "atoms": total_atoms}


def ingest_file(ipc, llm, path: Path, verbose: bool = True, domain: str = DEFAULT_DOMAIN) -> dict:
    text = path.read_text(encoding="utf-8", errors="replace")
    return ingest_text(ipc, llm, text, source_tag=path.name, verbose=verbose, domain=domain)
