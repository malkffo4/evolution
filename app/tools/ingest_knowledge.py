#!/usr/bin/env python3
# app/tools/ingest_knowledge.py
"""
Асинхронный параллельный Knowledge Ingestion Pipeline.
Разбивает текст на чанки и парсит их через облачные API одновременно,
что ускоряет загрузку целых книг в десятки раз.

Зависит от: pip install httpx tenacity tqdm
"""

import asyncio
import argparse
import sys
from pathlib import Path

# Чтобы импорты из корня app работали корректно
APP_DIR = Path(__file__).resolve().parents[1]
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

try:
    from tqdm.asyncio import tqdm
except ImportError:
    print("[ERROR] Please install dependencies: pip install tqdm httpx tenacity", file=sys.stderr)
    sys.exit(1)

from core.sdk import CoreClient, chunk_text, parse_json
from core.llm import LLMClient
from knowledge.prompts import EXTRACTION_PROMPT

# Максимальное количество одновременных запросов к API
MAX_CONCURRENT_TASKS = 5

async def extract_and_learn_chunk(core: CoreClient, llm: LLMClient, chunk: str, source_tag: str, sem: asyncio.Semaphore) -> int:
    """Асинхронный воркер: запрашивает LLM и отправляет извлеченные атомы в ядро."""
    async with sem:  # Ограничиваем параллелизм
        prompt = EXTRACTION_PROMPT.format(chunk=chunk)
        raw_response = await llm.aquery(prompt, json_mode=True)
        data = parse_json(raw_response)

        if not data or "atoms" not in data or not isinstance(data["atoms"], list):
            return 0

        atoms = data["atoms"]
        if not atoms:
            return 0

        # Добавляем provenance (источник)
        for a in atoms:
            a.setdefault("context", source_tag)

        # Отправляем в C-ядро асинхронно. CoreClient внутри использует threading.Lock,
        # так что IPC-сокет не сломается от одновременных записей.
        resp = await core.learn_async({"atoms": atoms})
        if resp.get("name") == "error":
            print(f"\n[ERROR] Core rejected atoms: {resp.get('payload')}", file=sys.stderr)
            return 0

        return len(atoms)

async def ingest_file_async(core: CoreClient, llm: LLMClient, path: Path, source_tag: str) -> dict:
    """Читает файл, режет на чанки и запускает пул асинхронных воркеров."""
    text = path.read_text(encoding="utf-8", errors="replace")
    chunks = chunk_text(text)

    print(f"[ingest] Начинаю парсинг '{path.name}': {len(chunks)} чанков, {len(text)} символов.")
    print(f"[ingest] Модель: {llm.provider} ({llm.model})")

    # Семафор ограничивает количество параллельных HTTP запросов к LLM провайдеру
    sem = asyncio.Semaphore(MAX_CONCURRENT_TASKS)

    tasks = [
        extract_and_learn_chunk(core, llm, chunk, source_tag, sem)
        for chunk in chunks
    ]

    # Запускаем все задачи с красивым прогресс-баром
    results = await tqdm.gather(*tasks, desc="Извлечение знаний", unit="чанк")

    total_atoms = sum(results)
    return {"file": str(path), "chunks": len(chunks), "atoms": total_atoms}


def main():
    ap = argparse.ArgumentParser(description="Parallel NeuroCore Knowledge Ingestion")
    ap.add_argument("path", type=Path, help="Путь к текстовому файлу (.txt, .md)")
    ap.add_argument("--provider", default="auto", choices=["auto", "ollama", "openai", "gemini", "deepseek", "anthropic"])
    ap.add_argument("--source", default=None, help="Тег источника (по умолчанию имя файла)")
    ap.add_argument("--workers", type=int, default=5, help="Количество параллельных потоков (по умолчанию 5)")
    args = ap.parse_args()

    if not args.path.exists():
        sys.exit(f"[ERROR] Файл не найден: {args.path}")

    global MAX_CONCURRENT_TASKS
    MAX_CONCURRENT_TASKS = args.workers

    # Инициализация клиентов
    core = CoreClient().connect()
    llm = LLMClient(provider=args.provider)
    source_tag = args.source or args.path.name

    # Запуск асинхронного цикла
    try:
        result = asyncio.run(ingest_file_async(core, llm, args.path, source_tag))
        print(f"\n[ingest] ГОТОВО! Успешно загружено {result['atoms']} атомов из {result['chunks']} чанков в HyperMemory.")
    except KeyboardInterrupt:
        print("\n[ingest] Прервано пользователем.")
    finally:
        core.close()

if __name__ == "__main__":
    main()
