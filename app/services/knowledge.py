# app/services/knowledge.py
import json
from core.base_service import BaseService

class KnowledgeService(BaseService):
    def run(self, *args, **kwargs):
            """Точка входа BaseService. Делегирует на learn_from_text."""
            text = kwargs.get("text") if "text" in kwargs else (args[0] if args else None)
            if not text:
                raise ValueError("KnowledgeService.run() requires 'text'")
            return self.learn_from_text(text)

    def get_graph_context(self, keyword: str) -> str:
        """Запрашивает семантический подграф у C-ядра"""
        try:
            response = self.ipc.request("retrieve", {"query": keyword})
            payload = json.loads(response.get("payload", "{}"))
            atoms = payload.get("atoms", [])
            if not atoms:
                return ""
            lines = []
            for a in atoms:
                pid = a.get("process")
                args = a.get("args", [])
                lines.append(f"  process={pid} args={args}")
            return "\n".join(lines)
        except Exception:
            return ""

    def learn_from_text(self, text: str):
        if not self.llm:
            return {"ok": False, "error": "LLM not configured"}
        prompt = f"Извлеки факты (nodes и edges) из следующего текста. Верни строго JSON с ключами 'nodes' и 'edges'.\n\n{text}"
        raw = self.llm.query(prompt)
        try:
            graph = json.loads(raw)
            self.send_to_core(graph)
            return {"ok": True, "nodes": len(graph.get("nodes", [])), "edges": len(graph.get("edges", []))}
        except json.JSONDecodeError:
            return {"ok": False, "error": "LLM вернул невалидный JSON"}
