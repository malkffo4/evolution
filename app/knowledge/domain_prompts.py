# app/knowledge/domain_prompts.py

import json
from knowledge.domain_ns import namespace_atom_args
from knowledge.embeddings import augment_atoms_with_entity_embeddings

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

KILLCHAIN_STAGES = (
    "Reconnaissance", "Weaponization", "Delivery", "Exploitation",
    "Installation", "Command_and_Control", "Actions_on_Objectives",
)
KILLCHAIN_RELATIONS = ("PRECEDES", "ENABLES", "HAS_TECHNIQUE", "MITIGATED_BY", "DETECTED_BY")

KILLCHAIN_EXTRACTION_PROMPT = """Ты — экстрактор модели Cyber Kill Chain.

СТРОГОЕ ОГРАНИЧЕНИЕ СЛОВАРЯ (критично):
- args[0]/args[1] для отношения PRECEDES/ENABLES ДОЛЖНЫ быть ТОЛЬКО
  одним из семи канонических значений: {stages}
- process ДОЛЖЕН быть ТОЛЬКО одним из: {relations}
- Если текст описывает что-то, не укладывающееся в этот словарь (конкретная
  техника, инструмент, CVE) — используй HAS_TECHNIQUE(stage, technique_name),
  НЕ придумывай новый stage и НЕ придумывай новое отношение.
- Если не уверен, что текст явно это утверждает — не извлекай атом вообще.
  Лучше пропустить факт, чем сгенерировать несуществующий.

{base_schema}
Текст:
\"\"\"{{chunk}}\"\"\"
""".format(stages=", ".join(KILLCHAIN_STAGES), relations=", ".join(KILLCHAIN_RELATIONS),
           base_schema="{}")

def validate_killchain_atoms(atoms: list) -> tuple[list, list]:
    """Детерминированная пост-фильтрация вместо доверия только промпту —
    LLM стохастична, валидатор — нет (Principle 10: "любое знание может
    быть пересмотрено", но в граф должно попадать только то, что прошло
    проверку схемы). Возвращает (valid, rejected)."""
    valid, rejected = [], []
    for a in atoms:
        proc = a.get("process")
        args = a.get("args", [])
        if proc in ("PRECEDES", "ENABLES"):
            ok = len(args) == 2 and args[0] in KILLCHAIN_STAGES and args[1] in KILLCHAIN_STAGES
        elif proc == "HAS_TECHNIQUE":
            ok = len(args) == 2 and args[0] in KILLCHAIN_STAGES
        elif proc in ("MITIGATED_BY", "DETECTED_BY"):
            ok = len(args) == 2   # техника-специфичные, словарь стадий не применим к обоим args
        else:
            ok = False            # процесс вне объявленного словаря — по умолчанию отклоняем
        (valid if ok else rejected).append(a)
    return valid, rejected


def ingest_domain(ipc, llm, text: str, prompt_template: str, source_tag: str, domain: str,
                   validator=None):
    from knowledge.deep_extractor import chunk_text, _parse_json
    total_valid, total_rejected = 0, 0
    for chunk in chunk_text(text):
        raw = llm.query(prompt_template.format(chunk=chunk[:3000], base_schema="{}"), json_mode=True)
        data = _parse_json(raw) or {}
        atoms = data.get("atoms", [])

        if validator:
            atoms, rejected = validator(atoms)
            total_rejected += len(rejected)
            if rejected:
                print(f"[Critic/ingest] отклонено {len(rejected)} атом(ов) вне словаря: "
                      f"{[r.get('process') for r in rejected]}", file=sys.stderr)

        for a in atoms:
            a.setdefault("context", source_tag)
            namespace_atom_args(a, domain)
        atoms = augment_atoms_with_entity_embeddings(atoms)

        if atoms:
            ipc.command("learn", json.dumps({"atoms": atoms}))
            total_valid += len(atoms)
    return {"source": source_tag, "atoms_accepted": total_valid, "atoms_rejected": total_rejected}


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
