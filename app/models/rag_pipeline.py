#!/usr/bin/env python3
import json
import sys
from pathlib import Path

# Подключаем твой IPC клиент
sys.path.append(str(Path(__file__).resolve().parents[1]))
from runtime.ipc import IPCClient
from models.ollama import Local_LLM

class NeuroSymbolicRAG:
    def __init__(self):
        self.ipc = IPCClient()
        self.llm = Local_LLM()
        self.ipc.connect()

    def get_graph_context(self, keyword: str) -> str:
        """Запрашивает семантический подграф у C-ядра"""
        print(f"[RAG] Запрашиваю жесткую аксиоматику графа для '{keyword}'...")
        response = self.ipc.request("retrieve", {"query": keyword.lower()})

        payload_raw = response.get("payload", "")
        if not payload_raw or "error" in payload_raw:
            print(f"[RAG] Ядро не знает о '{keyword}' или узел изолирован.")
            return ""

        try:
            graph_data = json.loads(payload_raw)
            nodes = graph_data.get("nodes", [])
            edges = graph_data.get("edges", [])

            context_lines = []
            for edge in edges:
                src = edge.get('source')
                tgt = edge.get('target')
                rel = edge.get('relation', 'СВЯЗАН_С')

                # Ищем имена по ID
                src_label = next((n.get('label', src) for n in nodes if n.get('id') == src), src)
                tgt_label = next((n.get('label', tgt) for n in nodes if n.get('id') == tgt), tgt)

                context_lines.append(f"- {src_label} [{rel}] {tgt_label}")

            return "\n".join(context_lines)

        except json.JSONDecodeError:
            return ""

    def answer_question(self, question: str, concept_keyword: str):
        """Полный цикл: Извлечение из Графа (C) -> Промпт -> Мышление (Python LLM)"""
        # 1. Извлекаем сырые аксиомы из LMDB
        context = self.get_graph_context(concept_keyword)

        if not context:
            return self.llm._ask_ollama_for_frames(question) # Fallback без графа

        # 2. Формируем нейро-символический промпт
        prompt = open('prompts/rag_neuro_symbolic_search.txt', 'r').read().format(question=question)

        print("\n[RAG] Собранный контекст отправлен в LLM. Ожидаем ответ...")

        # Используем существующий метод из твоего ollama.py
        payload = {
            "model": self.llm.model,
            "prompt": prompt,
            "stream": False
        }

        import requests
        try:
            resp = requests.post(self.llm.api, json=payload, timeout=120)
            return resp.json().get("response", "Нет ответа от LLM.")
        except Exception as e:
            return f"Ошибка LLM: {e}"

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Использование: python3 rag_pipeline.py <ключевое_слово_для_графа> \"Твой вопрос?\"")
        sys.exit(1)

    keyword = sys.argv[1]
    question = sys.argv[2]

    rag = NeuroSymbolicRAG()
    answer = rag.answer_question(question, keyword)

    print("\n\033[92m[ОТВЕТ ИИ]:\033[0m")
    print(answer)
