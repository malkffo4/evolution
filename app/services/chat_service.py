# app/services/chat_service.py
import re
from knowledge.rag_pipeline import NeuroSymbolicRAG
from core.llm import LLMClient

STOPWORDS = {"как", "что", "почему", "это", "для", "чем", "или", "если", "when", "what", "how"}


def _extract_keywords(text: str, top_k: int = 5) -> list[str]:
    words = re.findall(r"[a-zA-Zа-яА-Я0-9_]{3,}", text.lower())
    seen, out = set(), []
    for w in words:
        if w in STOPWORDS or w in seen:
            continue
        seen.add(w)
        out.append(w)
    return out[:top_k]


class ChatService:
    def __init__(self, ipc_client, llm: LLMClient = None):
        self.ipc = ipc_client
        self.llm = llm or LLMClient()
        self.rag = NeuroSymbolicRAG(ipc_client)

    def answer(self, user_text: str) -> str:
        keywords = _extract_keywords(user_text)
        context = self.rag.get_context_for_keywords(keywords)

        if context:
            prompt = (
                "Используй СТРОГО факты из базы знаний ниже. Если фактов "
                "недостаточно для полного ответа — прямо скажи, что именно неизвестно.\n\n"
                f"Факты:\n{context}\n\nВопрос: {user_text}"
            )
        else:
            prompt = f"В базе знаний нет фактов по теме. Ответь исходя из общих знаний.\nВопрос: {user_text}"

        return self.llm.query(prompt, json_mode=False)
