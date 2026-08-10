# app/tools/ingest_knowledge.py
"""
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
        try:
            # Пробуем быстрый асинхронный вызов (теперь он знает про кэш)
            raw_response = await llm.aquery(prompt, json_mode=True)
        except Exception as e:
            print(f"\n[WARN] Асинхронный запрос упал ({e}). Переключаемся на каскадный фоллбэк...", file=sys.stderr)
            # Если падает, вызываем llm.query в отдельном потоке — он сам переберет все остальные провайдеры
            raw_response = await asyncio.to_thread(llm.query, prompt, json_mode=True)

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

    # ФИКС: Если выбран web_*, отключаем асинхронность и работаем синхронно,
    # чтобы мог спокойно вбивать данные в одну вкладку без конфликтов.
    if llm.provider.startswith("web_"):
        print("[ingest] Web-режим. Обрабатываем строго последовательно (1 поток)...")
        total_atoms = 0
        for i, chunk in enumerate(chunks):
            print(f"  -> Отправка чанка {i+1}/{len(chunks)} в браузер...")
            prompt = EXTRACTION_PROMPT.format(chunk=chunk)

            # Используем прямой СИНХРОННЫЙ запрос
            raw_response = llm.query(prompt, json_mode=True)
            data = parse_json(raw_response)

            if data and "atoms" in data and data["atoms"]:
                atoms = data["atoms"]
                for a in atoms:
                    a.setdefault("context", source_tag)
                resp = core.learn({"atoms": atoms})
                if resp.get("name") != "error":
                    total_atoms += len(atoms)
                    print(f"     ✅ Извлечено {len(atoms)} атомов.")
        return {"file": str(path), "chunks": len(chunks), "atoms": total_atoms}
    else:
        # Для нормального API (Ollama/OpenAI) используем параллелизм
        sem = asyncio.Semaphore(MAX_CONCURRENT_TASKS)
        tasks = [
            extract_and_learn_chunk(core, llm, chunk, source_tag, sem)
            for chunk in chunks
        ]
        results = await tqdm.gather(*tasks, desc="Knowledge extraction", unit="chunk")
        return {"file": str(path), "chunks": len(chunks), "atoms": sum(results)}

def main():
    ap = argparse.ArgumentParser(description="Parallel NeuroCore Knowledge Ingestion")
    ap.add_argument("path", type=Path, help="Путь к текстовому файлу (.txt, .md)")
    # Добавлены web_ опции в choices
    ap.add_argument("--provider", default="auto", choices=["auto", "ollama", "openai", "gemini", "deepseek", "anthropic", "web_chatgpt", "web_deepseek"])
    ap.add_argument("--model", default=None, help="Model LLM (по умолчанию Default)")
    ap.add_argument("--source", default=None, help="Тег источника (по умолчанию имя файла)")
    ap.add_argument("--workers", type=int, default=5, help="Количество параллельных потоков (по умолчанию 5)")
    args = ap.parse_args()

    if not args.path.exists():
        sys.exit(f"[ERROR] Файл не найден: {args.path}")

    global MAX_CONCURRENT_TASKS
    MAX_CONCURRENT_TASKS = args.workers

    # Инициализация клиентов
    core = CoreClient().connect()
    llm = LLMClient(provider=args.provider, model=args.model)
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
