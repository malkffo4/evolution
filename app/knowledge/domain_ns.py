# app/knowledge/domain_ns.py
"""
Namespace для сущностей: node_id = djb2_hash(строка), поэтому
"reaction" в химии и "reaction" в security-инциденте сегодня — один и
тот же физический узел. Фикс целиком на Python: хэшируем не голый
label, а "<domain>::<label>".

Namespace применяется ТОЛЬКО к args[] доменных фактов. НЕ применяется к:
  - process (CAUSES, IS_A, REQUIRES, HAS_ALGORITHM...) — общий словарь,
    по нему матчит CorePlanner/InductiveExtractor/ZeroShotComposer;
  - kind (relation/entity/rule/goal/event);
  - cause (ссылка на goal/episode/атом по системному id);
  - именам Goal/Algorithm из book_loader.py/bootstrap.py — это не
    контент книги, а системные идентификаторы.
"""

import re

NAMESPACE_SEPARATOR = "::"
DEFAULT_DOMAIN = "general"

# Единый список доменов проекта, чтобы retrieval.py перебирал те же
# имена, что использует ingestion, и не плодил "cyber"/"security" как
# два разных домена по опечатке.
KNOWN_DOMAINS = (
    "cybersec",     # OWASP, HackerOne/HTB writeups, web pentest
    "networking",   # RFC, протоколы
    "programming",  # языки, алгоритмы, паттерны
    "culinary",
    "chemistry",
    DEFAULT_DOMAIN,
)

_NUMERIC_RE = re.compile(r"^-?\d+(\.\d+)?$")
_WS_RE = re.compile(r"\s+")


def _looks_numeric(value: str) -> bool:
    return bool(_NUMERIC_RE.match(value.strip()))


def canonicalize_label(label: str) -> str:
    """lower + trim + схлопывание пробелов. Не решает coreference
    ("SQLi" vs "SQL Injection" всё ещё разные узлы), но убирает самый
    частый источник промахов — регистр/пробелы от LLM-экстрактора."""
    return _WS_RE.sub(" ", label.strip()).lower()


def namespace_entity(label: str, domain: str = DEFAULT_DOMAIN) -> str:
    """label -> "<domain>::<canonical label>". Числа и уже
    namespace'нутые строки пропускаются как есть (идемпотентно)."""
    if not isinstance(label, str) or not label:
        return label
    if _looks_numeric(label):
        return label
    if NAMESPACE_SEPARATOR in label:
        return label
    domain = (domain or DEFAULT_DOMAIN).strip().lower() or DEFAULT_DOMAIN
    return f"{domain}{NAMESPACE_SEPARATOR}{canonicalize_label(label)}"


def strip_namespace(label: str) -> tuple[str, str]:
    """"<domain>::<label>" -> (domain, label)."""
    if not isinstance(label, str) or NAMESPACE_SEPARATOR not in label:
        return (DEFAULT_DOMAIN, label)
    domain, _, rest = label.partition(NAMESPACE_SEPARATOR)
    return (domain, rest)


def namespace_atom_args(atom: dict, domain: str) -> dict:
    """Применяет namespace ТОЛЬКО к atom['args']. Мутирует atom in-place
    и возвращает его же (для удобства в list comprehension/for)."""
    args = atom.get("args")
    if isinstance(args, list):
        atom["args"] = [
            namespace_entity(a, domain) if isinstance(a, str) else a
            for a in args
        ]
    return atom
