# app/knowledge/domain_prompts.py

SECURITY_EXTRACTION_PROMPT = """Ты — экстрактор методологии кибербезопасности.
Извлекай ТОЛЬКО методологический уровень: классы уязвимостей (CWE), техники
разведки, категории митигаций, взаимосвязи "что от чего зависит". НЕ извлекай
готовые payload'ы, эксплойт-код или пошаговые инструкции атаки — только
структуру знаний (что производит что, что требует что, что чем митигируется).

roles: PRODUCES | REQUIRES | CAUSES | MITIGATES | DETECTS | HAS_PROPERTY

properties{{}} — произвольные метаданные: cwe_id, cvss_score, affected_component,
recon_technique, service_type. Значения только описательные, не эксплойт-код.

{base_schema}

Текст:
\"\"\"{chunk}\"\"\"
"""

CULINARY_EXTRACTION_PROMPT = """Ты — экстрактор кулинарной химии.
roles: PRODUCES (ингредиент/реакция → вкус/текстура) | REQUIRES (нужна
температура/время/техника) | CONTRADICTS (несочетаемые вкусы/текстуры) |
CAUSES (реакция Майяра, карамелизация, эмульгирование) | HAS_PROPERTY.

properties{{}}: temperature_c, time_min, ph_range, flavor_compound,
maillard_threshold, fat_content.

{base_schema}

Текст:
\"\"\"{chunk}\"\"\"
"""

CHEMISTRY_EXTRACTION_PROMPT = """Ты — экстрактор физической/органической химии.
roles: PRODUCES | REQUIRES | CAUSES | CONTRADICTS (несовместимые реагенты) |
HAS_PROPERTY.

properties{{}}: activation_energy, boiling_point, molarity, catalyst,
reaction_type, exothermic (bool).

{base_schema}

Текст:
\"\"\"{chunk}\"\"\"
"""

def ingest_domain(ipc, llm, text: str, prompt_template: str, source_tag: str):
    from knowledge.deep_extractor import chunk_text, _parse_json
    for chunk in chunk_text(text):
        prompt = prompt_template.format(chunk=chunk[:3000], base_schema=BASE_SCHEMA)
        raw = llm.query(prompt, json_mode=True)
        data = _parse_json(raw) or {}
        atoms = data.get("atoms", [])
        for a in atoms:
            a.setdefault("context", source_tag)
        if atoms:
            ipc.command("learn", json.dumps({"atoms": atoms}))


def build_contradiction_pattern():
    return {
        "type": "hyper_pattern",
        "pattern_id": 2,
        "vars": [],
        "conditions": [
            {"process": "CONTRADICTS", "args": [{"const": "$A"}, {"const": "$B"}]}
        ]
    }

# app/knowledge/domain_prompts.py — add to every atom before learn()
# a["truth"] = {"mean": a.get("truth", {}).get("mean", 1.0), "confidence": 0.4}  # book-sourced, unverified
# a["properties"] = {**a.get("properties", {}), "source_kind": "book_claim", "book": source_tag}
