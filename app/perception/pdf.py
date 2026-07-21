# app/perception/pdf.py
import os
import sys
import json
import asyncio
import aiohttp
import fitz  # PyMuPDF # pdfplumber
from pathlib import Path

# Конфигурация
OLLAMA_API = "http://localhost:11434/api/generate"
MODEL = "qwen2.5-coder:1.5b"
SOCKET_PATH = "/tmp/evolution.sock"
CONCURRENCY_LIMIT = 5  # Количество параллельных потоков чтения книг

# Промпт для извлечения фреймов и цепочек атак
PROMPT_TEMPLATE = """
Ты — эксперт по кибербезопасности и системный аналитик. Прочитай фрагмент текста и извлеки из него:
1. Концепты (nodes): сущности, технологии, уязвимости, утилиты. Укажи для них:
   - "id" (в формате snake_case)
   - "label" (название на русском или английском)
   - "danger" (число от 0.0 до 1.0, опасность)
   - "utility" (число от 0.0 до 1.0, полезность для решения задач)
2. Логические связи (edges): причинно-следственные связи, зависимости, переходы состояний.
   - "source" (id источника)
   - "target" (id цели)
   - "relation" (тип связи на английском в верхнем регистре, например: REQUIRES, LEADS_TO, EXPLOITS)

Верни СТРОГО валидный JSON без разметки markdown и комментариев.
Формат ответа:
{{
  "nodes": [
    {{"id": "lfi_vuln", "label": "LFI уязвимость", "danger": 0.8, "utility": 0.3}}
  ],
  "edges": [
    {{"source": "lfi_vuln", "target": "source_code_leak", "relation": "ALLOWS_READING"}}
  ]
}}

Текст для анализа:
"{text_chunk}"
"""

async def extract_knowledge_from_chunk(session, text_chunk):
    prompt = PROMPT_TEMPLATE.format(text_chunk=text_chunk)
    payload = {
        "model": MODEL,
        "prompt": prompt,
        "format": "json",
        "stream": False
    }
    try:
        async with session.post(OLLAMA_API, json=payload, timeout=180) as resp:
            if resp.status == 200:
                result = await resp.json()
                return result.get("response", "")
    except Exception as e:
        print(f"[ERROR] Не удалось обработать чанк: {e}", file=sys.stderr)
    return None

async def send_to_c_core(data_json):
    """Отправка структурированного JSON-графа в C-ядро по Unix-сокету"""
    try:
        reader, writer = await asyncio.open_unix_connection(SOCKET_PATH)
        packet = {
            "type": "command",
            "name": "learn",
            "payload": {
                "graph": data_json
            }
        }
        writer.write((json.dumps(packet) + "\n").encode())
        await writer.drain()
        writer.close()
        await writer.wait_closed()
    except Exception as e:
        print(f"[ERROR] Ошибка передачи по IPC в C-ядро: {e}", file=sys.stderr)

async def process_pdf(filepath, session, semaphore):
    async with semaphore:
        print(f"[PROCESS] Начинаю поглощение книги: {filepath.name}")
        try:
            doc = fitz.open(filepath)
            for page_num in range(len(doc)):
                text = doc[page_num].get_text()
                if len(text.strip()) < 100:
                    continue

                # Делим страницу на блоки по ~1500 символов
                chunks = [text[i:i+1500] for i in range(0, len(text), 1200)]
                for chunk in chunks:
                    extracted_json = await extract_knowledge_from_chunk(session, chunk)
                    if extracted_json:
                        # Проверяем на валидность и шлем в Си-ядро
                        try:
                            parsed = json.loads(extracted_json)
                            if "nodes" in parsed and "edges" in parsed:
                                await send_to_c_core(parsed)
                        except json.JSONDecodeError:
                            continue
            print(f"[SUCCESS] Книга полностью запечена в память: {filepath.name}")
        except Exception as e:
            print(f"[ERROR] Ошибка разбора PDF {filepath.name}: {e}", file=sys.stderr)

async def main(books_dir):
    books_path = Path(books_dir)
    pdf_files = list(books_path.glob("**/*.pdf"))
    print(f"[SYSTEM] Найдено {len(pdf_files)} книг для когнитивного поглощения.")

    semaphore = asyncio.Semaphore(CONCURRENCY_LIMIT)
    async with aiohttp.ClientSession() as session:
        tasks = [process_pdf(f, session, semaphore) for f in pdf_files]
        await asyncio.gather(*tasks)

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Использование: python3 ingest_books.py <путь_к_папке_с_книгами>")
        sys.exit(1)

    asyncio.run(main(sys.argv[1]))
