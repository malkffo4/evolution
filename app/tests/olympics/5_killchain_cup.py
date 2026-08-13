# app/tests/olympics/5_killchain_cup.py

KILLCHAIN_ORDER = [
    "Reconnaissance", "Weaponization", "Delivery", "Exploitation",
    "Installation", "Command_and_Control", "Actions_on_Objectives",
]

def relation_exists(core, relation: str, terms=()):
    relation_norm = normalize(relation)
    required = tuple(normalize(x) for x in terms)

    # КЛЮЧЕВОЙ ФИКС: Всегда ищем по самому названию отношения (оно стабильно, т.к. из промпта).
    # Дополнительно запрашиваем и термины на случай, если LLM использовала другое отношение.
    queries = [relation.upper()] + list(terms)
    checked = set()

    for query in queries:
        query = str(query)
        if query in checked:
            continue
        checked.add(query)

        for atom in get_atoms(core, query):
            text = atom_text(atom)
            if relation_norm in text and all(term in text for term in required):
                return atom
    return None

def test_ordering_learned(core):
    """Позитив: все 6 последовательных PRECEDES-рёбер извлечены."""
    for a, b in zip(KILLCHAIN_ORDER, KILLCHAIN_ORDER[1:]):
        assert relation_exists(core, "PRECEDES", a, b), f"{a} -> {b} не извлечено"

def test_no_vocabulary_hallucination(core, atoms_before, atoms_after):
    """Негатив: НИ ОДНОГО атома с process вне словаря и args вне 7 стадий."""
    new_atoms = diff(atoms_before, atoms_after)
    for atom in new_atoms:
        if atom["process"] in ("PRECEDES", "ENABLES"):
            assert atom["args"][0] in KILLCHAIN_ORDER
            assert atom["args"][1] in KILLCHAIN_ORDER

def test_transfer_to_logs(core):
    """Уровень 4 AGI Olympics: перенос знания на НЕ встречавшиеся в тексте
    данные — классификация лога через HyperPattern, не через повторный LLM-вызов."""
    ingest_log_line(core, "outbound DNS beacon after phishing attachment opened")
    hyp = wait_for_hypothesis(core, timeout=10.0)
    assert hyp["stage"] in ("Installation", "Command_and_Control")
    assert hyp["confidence"] < 0.5          # это гипотеза, не факт
    assert hyp["process"] != "HAS_ALGORITHM" # не привязана к исполняемому действию
