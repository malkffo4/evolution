# app/knowledge/retrieval.py
"""
1. точный хэш с доменным префиксом (если domain передан)
2. точный хэш по остальным известным доменам по очереди
3. семантика: embed_text(query) -> IPC "find_similar" -> retrieve()
   по найденным соседям

Шаг 3 — самый дорогой (линейный скан simhash_index в C-ядре, LSH-
переделка отложена), поэтому вызывается только если 1-2 дали пусто.
"""
import json

from knowledge.domain_ns import namespace_entity, strip_namespace, KNOWN_DOMAINS
from knowledge.embeddings import embed_text

SEMANTIC_FALLBACK_TOP_K = 5


def _payload(resp: dict) -> dict:
    payload = resp.get("payload", {})
    if isinstance(payload, str):
        return json.loads(payload) if payload.strip() else {}
    return payload if isinstance(payload, dict) else {}


def _atoms_from_response(resp: dict) -> list:
    atoms = _payload(resp).get("atoms", [])
    return atoms if isinstance(atoms, list) else []


def _format_atoms(atoms: list, max_lines: int = 30) -> str:
    lines = []
    for a in atoms[:max_lines]:
        args = a.get("args", [])
        proc = a.get("process", "?")
        # legacy-формат из старого perceive_and_activate (EDGE, source/rel/target
        # тремя отдельными args) — недостижим текущим C-ядром (HYPER_VAL_SLOTS=2),
        # но оставляю для совместимости со старыми дампами базы.
        if proc == "EDGE" and len(args) >= 3:
            lines.append(f"{args[0]} --{args[1]}--> {args[2]}")
            continue
        clean_args = [strip_namespace(x)[1] if isinstance(x, str) else x for x in args]
        if len(clean_args) >= 2:
            lines.append(f"{clean_args[0]} --{proc}--> {clean_args[1]}")
        elif clean_args:
            lines.append(f"{proc}({clean_args[0]})")
    return "\n".join(lines)


def _domain_candidates(domain: str = None) -> list:
    if domain:
        return [domain] + [d for d in KNOWN_DOMAINS if d != domain]
    return list(KNOWN_DOMAINS)


def retrieve(ipc, query: str, domain: str = None, max_lines: int = 30) -> str:
    for d in _domain_candidates(domain):
        resp = ipc.request("retrieve", {"query": namespace_entity(query, d)})
        atoms = _atoms_from_response(resp)
        if atoms:
            return _format_atoms(atoms, max_lines)

    try:
        query_vec = embed_text(query)
        sim_resp = ipc.request("find_similar", {"embedding": query_vec, "top_k": SEMANTIC_FALLBACK_TOP_K})
    except Exception:
        return ""

    neighbors = _payload(sim_resp).get("results", [])
    if not isinstance(neighbors, list):
        return ""

    parts = []
    for n in neighbors:
        label = n.get("label") if isinstance(n, dict) else None
        if not label:
            continue
        atoms = _atoms_from_response(ipc.request("retrieve", {"query": label}))
        if not atoms:
            continue
        _, plain = strip_namespace(label)
        parts.append(f"[смысловой сосед: {plain}]\n{_format_atoms(atoms, max_lines)}")

    return "\n\n".join(parts)
