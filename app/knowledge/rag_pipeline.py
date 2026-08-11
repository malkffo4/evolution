# app/knowledge/rag_pipeline.py

from knowledge.retrieval import retrieve

class NeuroSymbolicRAG:
    """Единая точка получения фактов из C-ядра в текстовом виде с семантическим фоллбэком."""

    def __init__(self, ipc_client):
        self.ipc = ipc_client

    def get_graph_context(self, keyword: str, domain: str = None, max_lines: int = 30) -> str:
            return retrieve(self.ipc, keyword, domain=domain, max_lines=max_lines)

    def get_context_for_keywords(self, keywords: list[str]) -> str:
        parts = []
        for kw in keywords:
            ctx = self.get_graph_context(kw)
            if ctx:
                parts.append(f"Факты про '{kw}':\n{ctx}")
        return "\n\n".join(parts)
