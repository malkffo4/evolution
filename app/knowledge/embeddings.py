# app/knowledge/embeddings.py
"""
Офлайновый эмбеддинг для семантического fallback в retrieval.
НЕ нейросеть — hashing trick (тот же принцип, что в
sklearn.HashingVectorizer / Vowpal Wabbit): каждый токен
детерминированно хэшируется в одну из VECTOR_DIM координат со знаком,
итоговый вектор L2-нормализуется.

Почему так, а не эмбеддинг через LLM API:
  1. VECTOR_DIM в C-ядре зафиксирован в 128 (math/vector_math.h).
     API эмбеддингов обычно отдают 1536+ — нужна лишняя проекция.
  2. Один сетевой вызов на каждую сущность при загрузке книги —
     дорого и медленно на масштабе "терабайты мануалов" (TODO.md).
  3. Собственная философия проекта (docs/05_Understanding.md):
     "не вся обработка должна выполняться LLM" — там, где задачу
     решает детерминированный алгоритм, используем его.
  4. Память: 128 float = 512 байт на сущность, без внешнего рантайма.

Качество ниже настоящего sentence-эмбеддинга (это bag-of-tokens, без
понимания порядка слов), но для "нашёлся ли уже загруженный смысловой
сосед, если точного хэша нет" — этого достаточно, и это единственный
шаг retrieval, который срабатывает лишь как fallback после трёх более
дешёвых точных попыток (см. retrieval.py).

Использует core/sdk.py::djb2_hash — тот же алгоритм, что и C-ядро,
чтобы вся кодовая база хэшировала одинаково (единообразие, не
корректность: для самого эмбеддинга подошёл бы любой стабильный хэш).
"""

import math
import re
import requests
from core.sdk import djb2_hash
from knowledge.domain_ns import strip_namespace, NAMESPACE_SEPARATOR

VECTOR_DIM = 128  # ДОЛЖНО совпадать с C-ядром

_TOKEN_RE = re.compile(r"[a-zA-Zа-яА-Я0-9_]{2,}")

def embed_text(text: str, dim: int = VECTOR_DIM) -> list[float]:
    """
    Универсальный мультиязычный эмбеддинг через локальную Ollama (nomic-embed-text).
    Использует свойство Matryoshka Representation для сжатия до 128 измерений.
    Фоллбэк на Hashing Trick, если Ollama выключена.
    """
    try:
        # Дергаем локальную Ollama. Убедись, что выполнил `ollama run nomic-embed-text`
        resp = requests.post("http://localhost:11434/api/embeddings",
                             json={"model": "nomic-embed-text", "prompt": text},
                             timeout=2.0)
        if resp.status_code == 200:
            vec = resp.json().get("embedding", [])
            if vec and len(vec) >= dim:
                # Обрезаем до 128 (Matryoshka) и нормализуем
                sub_vec = vec[:dim]
                norm = math.sqrt(sum(v * v for v in sub_vec))
                if norm > 1e-9:
                    return [v / norm for v in sub_vec]
    except Exception:
        pass # Ollama недоступна, падаем в фоллбэк

    # Фоллбэк: Hashing trick (старый детерминированный алгоритм)
    vec = [0.0] * dim
    for tok in _TOKEN_RE.findall(text.lower()):
        idx = djb2_hash(tok) % dim
        sign = 1.0 if (djb2_hash(tok + "#sign") & 1) == 0 else -1.0
        vec[idx] += sign

    norm = math.sqrt(sum(v * v for v in vec))
    if norm > 1e-9:
        vec = [v / norm for v in vec]
    return vec

def entity_embedding_atom(namespaced_label: str) -> dict:
    domain, plain_label = strip_namespace(namespaced_label)
    return {
        "id": namespaced_label,
        "process": "ENTITY_EMBEDDING",
        "kind": "entity",
        "args": [namespaced_label, domain],
        "truth": {"mean": 1.0, "confidence": 1.0},
        "attention": {"sti": 0.1, "lti": 0.1},
        "embedding": embed_text(f"{domain} {plain_label}"),
    }

def augment_atoms_with_entity_embeddings(atoms: list) -> list:
    seen: set[str] = set()
    extra = []
    for a in atoms:
        for arg in a.get("args", []):
            if isinstance(arg, str) and NAMESPACE_SEPARATOR in arg and arg not in seen:
                seen.add(arg)
                extra.append(entity_embedding_atom(arg))
    return atoms + extra
