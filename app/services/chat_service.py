# app/services/chat_service.py
import re
import json
from knowledge.rag_pipeline import NeuroSymbolicRAG
from core.llm import LLMClient
from services.conversation_memory import ConversationMemory
from knowledge.semantic_compiler import SemanticCompiler  # Добавлено

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
        self.compiler = SemanticCompiler(self.llm)  # Инициализация компилятора

        self.session_id = "default_session"
        self.last_event_id = None
        self.chat_relevant_facts_pattern_id = 1  # Захардкоженный ID паттерна или загружаемый динамически

    def answer(self, user_text: str) -> str:
        # Реплику пользователя запоминаем сразу — даже если что-то ниже
        # упадёт, факт "он это спросил, и вот когда" уже останется в эпизоде.
        self.memory.remember_user(user_text)

        # 1. Компилируем реплику пользователя в графовое представление
        compile_res = self.compiler.compile(
            text=user_text,
            context_id=self.session_id,
            prev_event_id=self.last_event_id
        )

        # 2. Сохраняем новые атомы в LMDB (опирается на исправленный cmd_learn)
        if compile_res.atoms:
            self.ipc.command("learn", json.dumps({"atoms": compile_res.atoms}))

        self.last_event_id = compile_res.last_event_id or self.last_event_id

        # 3. Вызываем графовые операции через новый execute_op
        # Пример вызова паттерн-матчинга
        match_payload = {
            "op": "OP_MATCH_PATTERN",
            "regs": {
                "0": self.chat_relevant_facts_pattern_id, # r_pattern
                "1": 0                                    # r_ctx (без фильтра)
            },
            "args": [0, 1, 0, 3, 4, 20] # args: r_pattern, r_ctx, sp_base, r_count, r_varcount, max_results
        }
        match_resp = self.ipc.request("execute_op", match_payload)

        # Парсинг результатов из scratchpad можно добавить сюда для расширения графового контекста

        # Классический RAG-подход как фоллбэк
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

        # 4. Получаем ответ LLM
        reply = self.llm.query("\n".join(parts), json_mode=False)

        # Ответ ядра тоже становится частью его собственной истории.
        # 5. Сохраняем ответ ядра в граф
        reply_compile_res = self.compiler.compile(
            text=reply,
            context_id=self.session_id,
            prev_event_id=self.last_event_id
        )
        if reply_compile_res.atoms:
            self.ipc.command("learn", json.dumps({"atoms": reply_compile_res.atoms}))
            self.last_event_id = reply_compile_res.last_event_id or self.last_event_id

        self.memory.remember_core(reply)
        return reply

    def start_new_conversation(self, title: str = "Новый диалог"):
        self.memory.start_new_session(title)
        self.last_event_id = None
