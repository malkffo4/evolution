# app/services/conversation_memory.py
import json
import time
import hashlib
from core.ipc import IPCClient

class ConversationMemory:
    """
    Управляет диалоговой историей в гипер-памяти.
    Каждая реплика — атом с process = USER_SAYS или CORE_SAYS,
    args = [session_id, text, timestamp].
    """
    def __init__(self, ipc_client: IPCClient):
        self.ipc = ipc_client
        self.session_id = self._new_session_id("default")

    def _new_session_id(self, title: str) -> str:
        raw = f"{title}:{time.time()}"
        return hashlib.sha256(raw.encode()).hexdigest()[:16]

    def start_new_session(self, title: str = "Новый диалог"):
        self.session_id = self._new_session_id(title)

    def remember_user(self, text: str):
        self._save_utterance("USER_SAYS", text)

    def remember_core(self, text: str):
        self._save_utterance("CORE_SAYS", text)

    def _save_utterance(self, process: str, text: str):
        atom = {
            "process": process,
            "args": [self.session_id, text, str(time.time())],
            "confidence": 1.0
        }
        self.ipc.command("learn", json.dumps({"atoms": [atom]}))

    def format_history_for_prompt(self) -> str:
        """Извлекает все реплики сессии и форматирует для LLM."""
        # Используем retrieve по session_id как ключевому слову
        try:
            resp = self.ipc.request("retrieve", {"query": self.session_id})
            payload = resp.get("payload", {})
            if isinstance(payload, str):
                payload = json.loads(payload) if payload else {}
            atoms = payload.get("atoms", [])
        except Exception:
            return ""

        # Фильтруем атомы, где process == USER_SAYS или CORE_SAYS
        # и первый аргумент (session_id) совпадает (после хеширования).
        # Полагаемся на то, что retrieve вернул все атомы с участием session_id.
        utterances = []
        for a in atoms:
            if a.get("process") in ("USER_SAYS", "CORE_SAYS"):
                args = a.get("args", [])
                if len(args) >= 3:
                    speaker = "User" if a["process"] == "USER_SAYS" else "Core"
                    text = args[1]
                    ts = float(args[2]) if args[2] else 0
                    utterances.append((ts, f"[{speaker}]: {text}"))

        utterances.sort(key=lambda x: x[0])
        return "\n".join(u[1] for u in utterances)
