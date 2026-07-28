# app/services/mvp_agent.py
import json
import re

from knowledge.prompts import EXTRACTION_PROMPT

STOPWORDS = {
    "как", "что", "почему", "это", "для", "чем", "или", "если",
    "when", "what", "how", "is", "the", "a", "and", "или"
}


def _extract_keywords(text: str, top_k: int = 5) -> list:
    words = re.findall(r"[a-zA-Zа-яА-Я0-9_]{3,}", text.lower())
    seen, out = set(), []
    for w in words:
        if w in STOPWORDS or w in seen:
            continue
        seen.add(w)
        out.append(w)
    return out[:top_k]


class MvpAgent:
    """80/20 цикл: текст -> триплеты (LLM) -> запись в C-ядро (LMDB) ->
    подграф-контекст -> ответ LLM. Использует существующие IPCClient/LLMClient
    и уже рабочие обработчики cmd_learn / req_retrieve на стороне C."""

    def __init__(self, ipc_client, llm_client):
        self.ipc = ipc_client
        self.llm = llm_client

    # --- Шаг 2: LLM извлекает (Subject, Relation, Object) ---
    def extract_triplets(self, text: str) -> dict:
        prompt = EXTRACTION_PROMPT.format(chunk=text[:3000])
        raw = self.llm.query(prompt, json_mode=True)
        graph = self._parse_json(raw) or {}
        graph.setdefault("nodes", [])
        graph.setdefault("edges", [])
        return graph

    @staticmethod
    def _parse_json(raw: str):
        if not raw:
            return None
        raw = raw.strip()
        raw = re.sub(r"^```(json)?", "", raw).strip()
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

    # --- Шаг 3: запись графа в C-ядро (LMDB через cmd_learn -> perceive_and_activate) ---
    def store_graph(self, graph: dict) -> dict:
        if not graph.get("nodes") and not graph.get("edges"):
            return {"ok": False, "error": "empty graph, nothing to store"}
        return self.ipc.command("learn", json.dumps(graph))

    # --- Шаг 4: извлечение контекстного подграфа (req_retrieve -> hyper_retrieve_json) ---
    def retrieve_context(self, text: str, max_keywords: int = 5, max_lines: int = 30) -> str:
        lines = []
        for kw in _extract_keywords(text, max_keywords):
            try:
                resp = self.ipc.request("retrieve", {"query": kw})
                payload = resp.get("payload", {})
                if isinstance(payload, str):
                    payload = json.loads(payload) if payload else {}
                atoms = payload.get("atoms", [])
            except Exception:
                continue
            for a in atoms:
                args = a.get("args", [])
                proc = a.get("process", "?")
                if proc == "EDGE" and len(args) >= 3:
                    lines.append(f"{args[0]} --{args[1]}--> {args[2]}")

        seen = set()
        unique = [l for l in lines if not (l in seen or seen.add(l))]
        return "\n".join(unique[:max_lines])

    # --- Шаг 5: генерация ответа LLM с учётом контекста ---
    def generate_reply(self, user_text: str, context: str) -> str:
        if context:
            prompt = (
                "Используй факты из графа знаний ниже, если они относятся к вопросу. "
                "Если фактов недостаточно — отвечай исходя из общих знаний.\n\n"
                f"Факты:\n{context}\n\nВопрос пользователя: {user_text}"
            )
        else:
            prompt = user_text
        return self.llm.query(prompt, json_mode=False)

    # --- Полный цикл (шаги 1-5) ---
    def step(self, user_text: str) -> str:
        graph = self.extract_triplets(user_text)
        self.store_graph(graph)
        context = self.retrieve_context(user_text)
        return self.generate_reply(user_text, context)
