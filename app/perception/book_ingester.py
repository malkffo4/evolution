#!/usr/bin/env python3
# app/perception/book_ingester.py
import os
import json
import fitz  # PyMuPDF (pip install PyMuPDF)
import time
import sys
from pathlib import Path

# Подключаем IPC и LLM из твоего проекта
sys.path.append(str(Path(__file__).resolve().parents[1]))
from runtime.ipc import IPCClient
from models.ollama import Local_LLM

class BookIngester:
    def __init__(self, books_dir: str):
        self.books_dir = Path(books_dir)
        self.ipc = IPCClient()
        self.llm = Local_LLM()
        self.ipc.connect()

    def extract_text_from_pdf(self, pdf_path: str, max_pages: int = 50) -> str:
        """Читает PDF и возвращает текст. Ограничено max_pages для скорости тестов."""
        text = ""
        try:
            doc = fitz.open(pdf_path)
            for i, page in enumerate(doc):
                if i >= max_pages: break
                text += page.get_text("text") + "\n"
        except Exception as e:
            print(f"[ERROR] Ошибка чтения {pdf_path}: {e}")
        return text

    def extract_graph_from_text(self, text_chunk: str) -> dict:
        """Просит LLM превратить текст из книги в узлы и связи для графа"""
        system_prompt = (
            "Ты AI-аналитик по кибербезопасности. Твоя задача извлекать знания из текстов по пентесту (Active Directory, Web, Social Engineering).\n"
            "Верни СТРОГИЙ JSON-объект. Никакого лишнего текста.\n"
            "Узлы (nodes) - это технологии, уязвимости (CVE), утилиты (nmap, bloodhound), атаки (Kerberoasting, XSS) или концепции.\n"
            "Связи (edges) - это отношения между ними (ИСПОЛЬЗУЕТ, ЭКСПЛУАТИРУЕТ, ПРИВОДИТ_К, ЗАЩИЩАЕТ_ОТ).\n"
            "Формат:\n"
            "{\n"
            '  "nodes": [{"id": "bloodhound", "label": "BloodHound", "properties": {"type": "tool", "utility": 0.9}}],\n'
            '  "edges": [{"source": "bloodhound", "target": "active_directory", "relation": "ANALYZES"}]\n'
            "}\n"
            "Все id должны быть в нижнем регистре без пробелов (snake_case)."
        )

        prompt = f"Извлеки факты о пентесте из следующего отрывка книги:\n\n{text_chunk[:3000]}"

        try:
            import requests
            # Вызываем Ollama
            resp = requests.post(self.llm.api, json={
                "model": self.llm.model,
                "prompt": prompt,
                "system": system_prompt,
                "stream": False,
                "format": "json" # Заставляем выдать JSON
            }, timeout=120)

            result = resp.json().get("response", "{}")
            return json.loads(result)
        except Exception as e:
            print(f"[LLM ERROR] Не удалось извлечь граф: {e}")
            return {}

    def save_to_core(self, graph_data: dict):
        """Отправляет извлеченный граф в C-ядро через IPC (эндпоинт 'learn')"""
        if not graph_data or "nodes" not in graph_data:
            return

        try:
            resp = self.ipc.command("learn", json.dumps(graph_data))
            nodes_count = len(graph_data.get('nodes', []))
            edges_count = len(graph_data.get('edges', []))
            print(f"[Core] Успешно усвоено {nodes_count} концепций и {edges_count} связей. Ответ ядра: {resp.get('payload')}")
        except Exception as e:
            print(f"[IPC ERROR] Ошибка связи с ядром: {e}")

    def ingest_library(self):
        """Рекурсивно обходит папку с книгами и скармливает их в ядро"""
        print(f"[*] Начинаю сканирование библиотеки: {self.books_dir}")
        pdf_files = list(self.books_dir.rglob("*.pdf"))
        print(f"[*] Найдено {len(pdf_files)} PDF файлов.")

        # Для начала берем первые 3 файла (чтобы не ждать сутками)
        for pdf_file in pdf_files[:3]:
            print(f"\n[>] Читаю книгу: {pdf_file.name}")
            text = self.extract_text_from_pdf(str(pdf_file), max_pages=10) # Читаем первые 10 страниц для тестов

            # Бьем на куски по ~2000 символов
            chunks = [text[i:i+2000] for i in range(0, len(text), 2000)]

            for i, chunk in enumerate(chunks[:5]): # Берем 5 кусков
                print(f"    - Анализирую фрагмент {i+1}...")
                graph_data = self.extract_graph_from_text(chunk)
                if graph_data:
                    self.save_to_core(graph_data)
                time.sleep(1) # Пауза чтобы не перегреть LLM

if __name__ == "__main__":
    # Укажи тут реальный путь к своей папке
    TARGET_DIR = os.path.expanduser("~/Documents/books")
    ingester = BookIngester(TARGET_DIR)
    ingester.ingest_library()
    print("\n[+] Обучение завершено. База знаний в LMDB обновлена!")
