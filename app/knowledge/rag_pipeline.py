# app/knowledge/rag_pipeline.py
import json

class NeuroSymbolicRAG:
    """Единая точка получения фактов из C-ядра в текстовом виде.
    Полагается на резолв меток в hyper_retrieval.c (см. правку #2) —
    без неё этот класс получит только числовые хэши и будет бесполезен."""

    def __init__(self, ipc_client):
        self.ipc = ipc_client

    def get_graph_context(self, keyword: str, max_lines: int = 30) -> str:
        try:
            resp = self.ipc.request("retrieve", {"query": keyword})
            payload = resp.get("payload", {})
            if isinstance(payload, str):
                payload = json.loads(payload) if payload else {}
            atoms = payload.get("atoms", [])
        except Exception:
            return ""

        if not atoms:
            return ""

        lines = []
        for a in atoms[:max_lines]:
            args = a.get("args", [])
            proc = a.get("process", "?")

            if proc == "EDGE" and len(args) >= 3:
                # схема perceive_and_activate: args = [source, relation, target]
                lines.append(f"{args[0]} --{args[1]}--> {args[2]}")
            elif len(args) >= 2:
                lines.append(f"{args[0]} --{proc}--> {args[1]}")
            elif args:
                lines.append(f"{proc}({args[0]})")

        return "\n".join(lines)

    def get_context_for_keywords(self, keywords: list[str]) -> str:
        parts = []
        for kw in keywords:
            ctx = self.get_graph_context(kw)
            if ctx:
                parts.append(f"Факты про '{kw}':\n{ctx}")
        return "\n\n".join(parts)
