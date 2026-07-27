#!/usr/bin/env python3
import json
import sys
import re
from pathlib import Path
import requests

# Импортируем наш IPC клиент для отправки данных напрямую в C-ядро
sys.path.append(str(Path(__file__).resolve().parents[1]))
from runtime.ipc import IPCClient
from models.ollama import Local_LLM

class CognitiveLearner:
    def __init__(self):
        self.ipc = IPCClient()
        self.llm = Local_LLM()
        self.ipc.connect()
        
    def chunk_text(self, text: str, max_chars: int = 1500) -> list:
        """Нарезает текст на смысловые куски по абзацам или предложениям"""
        paragraphs = text.split("\n\n")
        chunks = []
        current_chunk = []
        current_length = 0
        
        for p in paragraphs:
            p = p.strip()
            if not p:
                continue
            if current_length + len(p) > max_chars:
                if current_chunk:
                    chunks.append("\n\n".join(current_chunk))
                current_chunk = [p]
                current_length = len(p)
            else:
                current_chunk.append(p)
                current_length += len(p)
                
        if current_chunk:
            chunks.append("\n\n".join(current_chunk))
        return chunks

    def extract_semantic_structure(self, text_chunk: str) -> dict:
        """
        Использует локальную LLM для извлечения чистой структуры связей.
        Просим модель вернуть строго валидный JSON.
        """
        system_instruction = (
            "Вы — аналитический модуль когнитивного ядра. Ваша задача — извлечь факты и структуру связей из текста.\n"
            "Вы должны вернуть строго JSON-объект со следующей структурой:\n"
            "{\n"
            "  \"nodes\": [ {\"id\": \"уникальное_имя_сущности_в_нижнем_регистре\", \"label\": \"Человеческое название\"} ],\n"
            "  \"edges\": [ {\"source\": \"id_субъекта\", \"target\": \"id_объекта\", \"relation\": \"ОТНОШЕНИЕ_В_ВЕРХНЕМ_РЕГИСТРЕ\"} ]\n"
            "}\n"
            "Используйте только факты из текста. Избегайте мусорных слов. Все id сущностей делайте строго в нижнем регистре на латинице или транслите (например, 'c_language', 'compiler', 'buffer_overflow'), чтобы ядро могло связать их хэши."
        )

        prompt = f"Извлеки структуру из следующего текста:\n\n{text_chunk}"
        
        payload = {
            "model": self.llm.model,
            "prompt": prompt,
            "system": system_instruction,
            "stream": False,
            "options": {
                "temperature": 0.1  # Низкая температура для строгой детерминированности
            }
        }
        
        try:
            response = requests.post(self.llm.api, json=payload, timeout=90)
            raw_text = response.json().get("response", "").strip()
            
            # Пытаемся найти JSON блок, если модель добавила лишний текст
            json_match = re.search(r'(\{.*?\})', raw_text, re.DOTALL)
            if json_match:
                return json.loads(json_match.group(1))
            return json.loads(raw_text)
        except Exception as e:
            print(f"[Learner ERROR] Не удалось распарсить структуру от LLM: {e}")
            return {"nodes": [], "edges": []}

    def learn_text(self, text: str):
        """Полный пайплайн: Нарезка -> Извлечение -> Упаковка в C-ядро"""
        chunks = self.chunk_text(text)
        print(f"[Learner] Текст разбит на {len(chunks)} частей. Начинаем извлечение структуры связей...")
        
        for idx, chunk in enumerate(chunks):
            print(f"\n[Learner] Обработка части {idx + 1}/{len(chunks)}...")
            structure = self.extract_semantic_structure(chunk)
            
            nodes = structure.get("nodes", [])
            edges = structure.get("edges", [])
            
            if not nodes and not edges:
                print("[Learner] Нет структуры для упаковки в этой части.")
                continue
                
            print(f"[Learner] Извлечено сущностей: {len(nodes)}, связей: {len(edges)}")
            
            # Отправляем напрямую в C-ядро через отлаженную шину IPC
            # На стороне C-ядра функция 'perceive_and_activate' / 'chat' примет структуру и запишет её в LMDB
            try:
                # Отправляем пакет 'perceive' для активации и записи графа
                # Если у тебя используется общий коннектор, мы упаковываем граф в payload
                packet = {
                    "nodes": nodes,
                    "edges": edges
                }
                
                # В зависимости от названия твоего эндпоинта записи отправляем запрос:
                resp = self.ipc.request("chat", {"text": f"/learn {json.dumps(packet)}"})
                
                # Или используем прямой метод, если в ядре реализован эндпоинт perceiver
                # resp = self.ipc.request("perceive", packet)
                
                print(f"[Learner] Ядро успешно приняло и упаковало структуру. Ответ: {resp.get('payload', 'OK')}")
            except Exception as e:
                print(f"[Learner ERROR] Ошибка отправки структуры в C-ядро: {e}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Использование:")
        print("  1. Прямой текст: python3 learner.py \"Твой текст для обучения\"")
        print("  2. Из файла:    python3 learner.py path/to/file.txt")
        sys.exit(1)
        
    input_data = sys.argv[1]
    text_to_learn = ""
    
    # Проверяем, передан ли путь к файлу
    file_path = Path(input_data)
    if file_path.exists() and file_path.is_file():
        print(f"[Learner] Читаем файл {file_path}...")
        text_to_learn = file_path.read_text(encoding="utf-8", errors="ignore")
    else:
        text_to_learn = input_data
        
    learner = CognitiveLearner()
    learner.learn_text(text_to_learn)
    print("\n\033[92m[ОБУЧЕНИЕ ЗАВЕРШЕНО] Все семантические связи упакованы в LMDB.\033[0m")