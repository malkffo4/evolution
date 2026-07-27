# app/services/chat_service.py
import re
from knowledge.rag_pipeline import NeuroSymbolicRAG
from core.llm import LLMClient
from services.conversation_memory import ConversationMemory

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
        self.memory = ConversationMemory(ipc_client)

    def answer(self, user_text: str) -> str:
        # Реплику пользователя запоминаем сразу — даже если что-то ниже
        # упадёт, факт "он это спросил, и вот когда" уже останется в эпизоде.
        self.memory.remember_user(user_text)

        keywords = _extract_keywords(user_text)
        graph_context = self.rag.get_context_for_keywords(keywords)
        history = self.memory.format_history_for_prompt()

        parts = []
        if graph_context:
            parts.append(
                "Используй СТРОГО факты из базы знаний ниже, если они относятся к вопросу. "
                f"Если фактов недостаточно — прямо скажи об этом.\n\nФакты:\n{graph_context}"
            )
        else:
            parts.append("В базе знаний нет специфичных фактов по теме — отвечай исходя из общих знаний.")

        if history:
            parts.append(f"\nПредыдущий разговор (учитывай его и время реплик):\n{history}")

        parts.append(f"\nНовое сообщение пользователя: {user_text}")

        reply = self.llm.query("\n".join(parts), json_mode=False)

        # Ответ ядра тоже становится частью его собственной истории.
        self.memory.remember_core(reply)
        return reply

    def start_new_conversation(self, title: str = "Новый диалог"):
        self.memory.start_new_session(title)
