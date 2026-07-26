#!/usr/bin/env python3
"""
ingest_pipeline.py (v2)
========================

Единый конвейер поглощения знаний для NeuroCore.

Изменения относительно v1:
  - Поддержка облачных LLM (Gemini, OpenAI) наравне с локальной Ollama —
    флаг --provider {ollama,openai,gemini}. Ключи берутся из переменных
    окружения GEMINI_API_KEY / OPENAI_API_KEY.
  - Параллельные запросы (--concurrency N). Для ollama по умолчанию 1
    (локальная модель и так съедает все ресурсы), для облака можно 4-8.
  - Пропуск фронт-страниц PDF (--skip-pages N) — титульники, списки
    ревьюеров/редакторов, оглавление съедают время впустую и не несут знаний.
  - Прогресс с ETA в консоли.

Использование
-------------
    # Локально (медленно, бесплатно)
    python3 ingest_pipeline.py book.pdf --kind book --provider ollama

    # Через Gemini (быстро, нужен ключ)
    export GEMINI_API_KEY=...
    python3 ingest_pipeline.py book.pdf --kind book --provider gemini \\
        --model gemini-2.0-flash --concurrency 6 --skip-pages 15

    # Через OpenAI
    export OPENAI_API_KEY=...
    python3 ingest_pipeline.py book.pdf --kind book --provider openai \\
        --model gpt-4o-mini --concurrency 6 --skip-pages 15
"""

import argparse
import ast
import hashlib
import json
import os
from dotenv import load_dotenv
import re
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

import requests

sys.path.append(str(Path(__file__).resolve().parent))
from runtime.ipc import IPCClient  # noqa: E402

# --------------------------------------------------------------------------- #
# Конфигурация провайдеров
# --------------------------------------------------------------------------- #

load_dotenv()  # читает .env

OLLAMA_API = "http://localhost:11434/api/generate"
OPENAI_API = "https://api.openai.com/v1/chat/completions"
GEMINI_API = "https://generativelanguage.googleapis.com/v1beta/models/{model}:generateContent"

DEFAULT_MODELS = {
    "ollama": "qwen2.5:3b",
    "openai": "gpt-4o-mini",
    "gemini": "gemini-2.0-flash",
}

REQUEST_TIMEOUT = 180
SLEEP_BETWEEN_CALLS = {"ollama": 0.3, "openai": 0.05, "gemini": 0.05}
MAX_RETRIES = 2

CHECKPOINT_PATH = Path(__file__).resolve().parent / ".ingest_checkpoint.json"

# --------------------------------------------------------------------------- #
# Промпты — одна согласованная схема на весь конвейер (совпадает с тем,
# что парсит core/src/perception/perception.c).
# --------------------------------------------------------------------------- #

LITERAL_PROMPT = """Ты — предельно точный и безэмоциональный экстрактор фактов.
Читай ТОЛЬКО то, что явно написано в тексте. НИЧЕГО не додумывай и не обобщай.
Игнорируй титульные данные, списки авторов/редакторов/ревьюеров, оглавления,
посвящения — извлекай только техническое содержание (концепции, уязвимости,
инструменты, техники, причинно-следственные связи).

Извлеки:
- "nodes": сущности, инструменты, техники, уязвимости, команды, концепции.
  Поля: id (snake_case, англ.), label (как в тексте), danger (0..1),
  utility (0..1, насколько полезно как инструмент/приём).
- "edges": явные отношения между узлами из текста.
  Поля: source, target, relation (CAUSES, USES, REQUIRES, EXPLOITS, PART_OF,
  LEADS_TO — заглавными, англ.).

Если в тексте нет технических фактов (титульный лист, благодарности, список
имён) — верни пустые списки.

Верни СТРОГО валидный JSON и ничего кроме него:
{{"nodes": [{{"id":"...", "label":"...", "danger":0.0, "utility":0.0}}],
  "edges": [{{"source":"...", "target":"...", "relation":"..."}}]}}

Текст:
\"\"\"{chunk}\"\"\"
"""

INTERPRET_PROMPT = """Ты помогаешь восстановить смысл собственных черновых заметок
автора по программированию/пентесту. Заметка написана "для себя", может быть
неполной, содержать сокращения или неочевидные ссылки на контекст.

Предложи от 1 до 3 РАЗНЫХ правдоподобных толкований того, что автор имел в виду
и зачем это записал (идея? баг? todo? наблюдение? план атаки? архитектурное
решение?). Для каждого укажи confidence (0..1).

Верни СТРОГО валидный JSON, формат гипер-атомов:
{{"atoms": [
  {{"process": "MEANS", "args": ["<заметка_кратко>", "<толкование>"], "confidence": 0.6}}
]}}

Заметка:
\"\"\"{chunk}\"\"\"
"""

# --------------------------------------------------------------------------- #
# Чекпоинт
# --------------------------------------------------------------------------- #


class Checkpoint:
    def __init__(self, path: Path):
        self.path = path
        self.done: set[str] = set()
        if path.exists():
            try:
                self.done = set(json.loads(path.read_text()))
            except Exception:
                self.done = set()

    def is_done(self, key: str) -> bool:
        return key in self.done

    def mark_done(self, key: str):
        self.done.add(key)
        self.path.write_text(json.dumps(list(self.done)))

    def reset(self):
        self.done = set()
        if self.path.exists():
            self.path.unlink()


def chunk_key(text: str, tag: str) -> str:
    return tag + ":" + hashlib.sha1(text.encode("utf-8", "ignore")).hexdigest()


# --------------------------------------------------------------------------- #
# LLM providers — единая точка вызова + устойчивый парсинг JSON
# --------------------------------------------------------------------------- #


def _repair_and_parse_json(raw: str) -> dict | None:
    raw = raw.strip()
    raw = re.sub(r"^```(json)?", "", raw).strip()
    raw = re.sub(r"```$", "", raw).strip()
    try:
        return json.loads(raw)
    except json.JSONDecodeError:
        pass
    match = re.search(r"\{.*\}", raw, re.DOTALL)
    if match:
        try:
            return json.loads(match.group(0))
        except json.JSONDecodeError:
            return None
    return None


def _call_ollama(prompt: str, model: str) -> str | None:
    payload = {"model": model, "prompt": prompt, "format": "json", "stream": False}
    resp = requests.post(OLLAMA_API, json=payload, timeout=REQUEST_TIMEOUT)
    resp.raise_for_status()
    return resp.json().get("response", "")


def _call_openai(prompt: str, model: str) -> str | None:
    key = os.getenv("OPENAI_API_KEY")
    if not key:
        raise RuntimeError("OPENAI_API_KEY не задан")
    headers = {"Authorization": f"Bearer {key}", "Content-Type": "application/json"}
    payload = {
        "model": model,
        "messages": [{"role": "user", "content": prompt}],
        "response_format": {"type": "json_object"},
        "temperature": 0.1,
    }
    resp = requests.post(OPENAI_API, headers=headers, json=payload, timeout=REQUEST_TIMEOUT)
    resp.raise_for_status()
    return resp.json()["choices"][0]["message"]["content"]


def _call_gemini(prompt: str, model: str) -> str | None:
    key = os.getenv("GEMINI_API_KEY")
    if not key:
        raise RuntimeError("GEMINI_API_KEY не задан")
    url = GEMINI_API.format(model=model) + f"?key={key}"
    payload = {
        "contents": [{"parts": [{"text": prompt}]}],
        "generationConfig": {"temperature": 0.1, "responseMimeType": "application/json"},
    }
    resp = requests.post(url, json=payload, timeout=REQUEST_TIMEOUT)
    resp.raise_for_status()
    data = resp.json()
    return data["candidates"][0]["content"]["parts"][0]["text"]


PROVIDER_CALLERS = {"ollama": _call_ollama, "openai": _call_openai, "gemini": _call_gemini}


def call_llm(prompt: str, provider: str, model: str) -> dict | None:
    caller = PROVIDER_CALLERS[provider]
    for attempt in range(MAX_RETRIES + 1):
        try:
            raw = caller(prompt, model)
            parsed = _repair_and_parse_json(raw) if raw else None
            if parsed is not None:
                return parsed
        except requests.exceptions.Timeout:
            print(f"  [{provider}] timeout (попытка {attempt + 1}/{MAX_RETRIES + 1})", file=sys.stderr)
        except Exception as e:
            print(f"  [{provider}] ошибка: {e}", file=sys.stderr)
        time.sleep(1.0)
    return None


# --------------------------------------------------------------------------- #
# Чанкинг
# --------------------------------------------------------------------------- #


def chunk_text(text: str, size: int, overlap: int = 80):
    text = re.sub(r"\n{3,}", "\n\n", text).strip()
    if not text:
        return
    start, n = 0, len(text)
    while start < n:
        end = min(start + size, n)
        if end < n:
            cut = text.rfind("\n", start, end)
            if cut == -1 or cut <= start + size // 2:
                cut = text.rfind(". ", start, end)
            if cut != -1 and cut > start + size // 3:
                end = cut + 1
        piece = text[start:end].strip()
        if len(piece) > 40:
            yield piece
        start = end - overlap if end - overlap > start else end


# --------------------------------------------------------------------------- #
# Source adapters
# --------------------------------------------------------------------------- #


def read_pdf(path: Path, skip_pages: int = 0) -> str:
    import fitz
    doc = fitz.open(path)
    pages = list(doc)[skip_pages:]
    return "\n".join(p.get_text() for p in pages)


def read_text_file(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="ignore")


def extract_code_structure(path: Path) -> dict:
    """Детерминированный (не-LLM) разбор кода — экономит вызовы LLM."""
    nodes, edges = [], []
    text = read_text_file(path)
    mod_id = re.sub(r"[^a-zA-Z0-9_]", "_", path.stem)

    if path.suffix == ".py":
        try:
            tree = ast.parse(text)
        except SyntaxError:
            return {"nodes": [], "edges": []}
        for node in ast.walk(tree):
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                fid = f"{mod_id}_{node.name}"
                nodes.append({"id": fid, "label": node.name, "danger": 0.0, "utility": 0.5})
                edges.append({"source": mod_id, "target": fid, "relation": "DEFINES"})
            elif isinstance(node, ast.ClassDef):
                cid = f"{mod_id}_{node.name}"
                nodes.append({"id": cid, "label": node.name, "danger": 0.0, "utility": 0.6})
                edges.append({"source": mod_id, "target": cid, "relation": "DEFINES"})
            elif isinstance(node, (ast.Import, ast.ImportFrom)):
                for alias in node.names:
                    dep = alias.name.split(".")[0]
                    dep_id = re.sub(r"[^a-zA-Z0-9_]", "_", dep)
                    nodes.append({"id": dep_id, "label": dep, "danger": 0.0, "utility": 0.3})
                    edges.append({"source": mod_id, "target": dep_id, "relation": "DEPENDS_ON"})
    else:
        for inc in re.findall(r'#include\s*[<"]([^">]+)[">]', text):
            dep_id = re.sub(r"[^a-zA-Z0-9_]", "_", inc)
            nodes.append({"id": dep_id, "label": inc, "danger": 0.0, "utility": 0.3})
            edges.append({"source": mod_id, "target": dep_id, "relation": "DEPENDS_ON"})
        for fn in re.findall(r'\b([a-zA-Z_][a-zA-Z0-9_]*)\s*\([^;{}]*\)\s*\{', text):
            fid = f"{mod_id}_{fn}"
            nodes.append({"id": fid, "label": fn, "danger": 0.0, "utility": 0.5})
            edges.append({"source": mod_id, "target": fid, "relation": "DEFINES"})

    if nodes:
        nodes.append({"id": mod_id, "label": path.name, "danger": 0.0, "utility": 0.4})
    return {"nodes": nodes, "edges": edges}


# --------------------------------------------------------------------------- #
# Прогресс
# --------------------------------------------------------------------------- #


class Progress:
    def __init__(self, total: int):
        self.total = total
        self.done = 0
        self.start = time.time()

    def tick(self):
        self.done += 1
        elapsed = time.time() - self.start
        rate = self.done / elapsed if elapsed > 0 else 0
        remaining = (self.total - self.done) / rate if rate > 0 else float("inf")
        eta_min = remaining / 60
        print(f"  [{self.done}/{self.total}] elapsed={elapsed/60:.1f}m ETA={eta_min:.1f}m", file=sys.stderr)


# --------------------------------------------------------------------------- #
# Основной конвейер
# --------------------------------------------------------------------------- #


class Ingestor:
    def __init__(self, provider: str, model: str, concurrency: int, dry_run: bool, checkpoint: Checkpoint):
        self.provider = provider
        self.model = model
        self.concurrency = concurrency
        self.dry_run = dry_run
        self.checkpoint = checkpoint
        self.ipc = None if dry_run else IPCClient()
        if self.ipc:
            self.ipc.connect()

    def send_graph(self, payload: dict):
        if not payload.get("nodes") and not payload.get("edges") and not payload.get("atoms"):
            return
        if self.dry_run:
            print(json.dumps(payload, ensure_ascii=False, indent=2))
            return
        # ВАЖНО: IPCClient в текущем виде не потокобезопасен (один сокет).
        # При параллельных запросах к LLM отправку в ядро всё равно
        # сериализуем через этот же вызов из основного потока (см. process_book).
        resp = self.ipc.command("learn", json.dumps(payload))
        print(f"  -> learn: {resp.get('payload', {})}")

    def _run_one(self, prompt: str) -> dict | None:
        return call_llm(prompt, self.provider, self.model)

    def process_book(self, path: Path, chunk_size: int, skip_pages: int):
        print(f"[BOOK] {path.name}")
        text = read_pdf(path, skip_pages) if path.suffix.lower() == ".pdf" else read_text_file(path)
        chunks = [
            (i, c) for i, c in enumerate(chunk_text(text, chunk_size))
            if not self.checkpoint.is_done(chunk_key(c, f"book:{path.name}"))
        ]
        if not chunks:
            print("  всё уже обработано (см. чекпоинт)")
            return
        progress = Progress(len(chunks))

        def work(item):
            i, chunk = item
            result = self._run_one(LITERAL_PROMPT.format(chunk=chunk))
            return i, chunk, result

        # Для ollama concurrency=1 обязателен (одна модель — один поток исполнения).
        # Для облака можно параллелить сами запросы к LLM, но отправку в C-ядро
        # всё равно делаем последовательно из главного потока (ниже).
        with ThreadPoolExecutor(max_workers=self.concurrency) as pool:
            futures = [pool.submit(work, item) for item in chunks]
            for fut in as_completed(futures):
                i, chunk, result = fut.result()
                if result:
                    self.send_graph(result)
                self.checkpoint.mark_done(chunk_key(chunk, f"book:{path.name}"))
                progress.tick()
                time.sleep(SLEEP_BETWEEN_CALLS[self.provider])

    def process_note(self, path: Path, chunk_size: int):
        print(f"[NOTE] {path.name}")
        text = read_text_file(path)
        for i, chunk in enumerate(chunk_text(text, min(chunk_size, 400))):
            lit_key = chunk_key(chunk, f"note-lit:{path.name}")
            int_key = chunk_key(chunk, f"note-int:{path.name}")

            if not self.checkpoint.is_done(lit_key):
                result = self._run_one(LITERAL_PROMPT.format(chunk=chunk))
                if result:
                    self.send_graph(result)
                self.checkpoint.mark_done(lit_key)
                time.sleep(SLEEP_BETWEEN_CALLS[self.provider])

            if not self.checkpoint.is_done(int_key):
                result = self._run_one(INTERPRET_PROMPT.format(chunk=chunk))
                if result:
                    self.send_graph(result)
                self.checkpoint.mark_done(int_key)
                time.sleep(SLEEP_BETWEEN_CALLS[self.provider])

    def process_code(self, path: Path):
        key = chunk_key(path.name + str(path.stat().st_mtime), f"code:{path}")
        if self.checkpoint.is_done(key):
            return
        print(f"[CODE] {path}")
        self.send_graph(extract_code_structure(path))
        self.checkpoint.mark_done(key)

    def close(self):
        if self.ipc:
            self.ipc.close()


EXT_BY_KIND = {
    "book": {".pdf", ".txt", ".md"},
    "note": {".txt", ".md"},
    "code": {".py", ".c", ".h"},
}


def main():
    ap = argparse.ArgumentParser(description="Единый конвейер поглощения знаний NeuroCore")
    ap.add_argument("path", type=str)
    ap.add_argument("--kind", choices=["book", "note", "code"], required=True)
    ap.add_argument("--provider", choices=["ollama", "openai", "gemini"], default="ollama")
    ap.add_argument("--model", default=None, help="по умолчанию берётся из DEFAULT_MODELS")
    ap.add_argument("--chunk-size", type=int, default=900)
    ap.add_argument("--skip-pages", type=int, default=0, help="сколько первых страниц PDF пропустить (титульник и т.п.)")
    ap.add_argument("--concurrency", type=int, default=None, help="1 для ollama, 4-8 для облака")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--reset", action="store_true")
    args = ap.parse_args()

    model = args.model or DEFAULT_MODELS[args.provider]
    concurrency = args.concurrency or (1 if args.provider == "ollama" else 5)

    checkpoint = Checkpoint(CHECKPOINT_PATH)
    if args.reset:
        checkpoint.reset()

    root = Path(args.path)
    exts = EXT_BY_KIND[args.kind]
    files = [root] if root.is_file() else sorted(
        p for p in root.rglob("*") if p.is_file() and p.suffix.lower() in exts
    )
    print(f"Найдено файлов: {len(files)} | provider={args.provider} model={model} concurrency={concurrency}")

    ingestor = Ingestor(provider=args.provider, model=model, concurrency=concurrency,
                         dry_run=args.dry_run, checkpoint=checkpoint)
    try:
        for path in files:
            try:
                if args.kind == "book":
                    ingestor.process_book(path, args.chunk_size, args.skip_pages)
                elif args.kind == "note":
                    ingestor.process_note(path, args.chunk_size)
                elif args.kind == "code":
                    ingestor.process_code(path)
            except KeyboardInterrupt:
                raise
            except Exception as e:
                print(f"[ERROR] {path}: {e}", file=sys.stderr)
    except KeyboardInterrupt:
        print("\nПрервано пользователем — прогресс сохранён в чекпоинте.")
    finally:
        ingestor.close()


if __name__ == "__main__":
    main()
