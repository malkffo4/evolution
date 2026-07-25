#!/usr/bin/env python3
"""
ingest_pipeline.py
===================

Единый конвейер поглощения знаний для NeuroCore.

Заменяет разрозненные и частично сломанные скрипты:
  app/perception/pdf.py
  app/perception/book_ingester.py
  app/perception/filesystem.py

Идея
----
1. Источник -> Source Adapter (PDF / TXT-MD заметки / код) -> текст/структура.
2. Код НЕ гоняем через LLM — парсим детерминированно (см. docs/05_Understanding.md:
   "Не вся обработка должна выполняться LLM"). Это экономит единственный
   дефицитный ресурс — время слабой локальной модели.
3. Обычный текст (книги) режем на маленькие чанки и извлекаем ЛИТЕРАЛЬНЫЕ факты
   в формате nodes/edges (тот же формат, что уже понимает C-ядро в perception.c).
4. Личные заметки (messy notes) прогоняются ДВАЖДЫ:
     - литеральный проход (что написано)
     - интерпретирующий проход (что автор, ВЕРОЯТНО, имел в виду — несколько
       гипотез с confidence, это прямо ложится в вашу модель Hypothesis)
5. Всё отправляется в C-ядро через уже существующий IPCClient (`learn`),
   ничего в ядре менять не нужно.
6. Чекпоинт на диске — обработка медленная (3B модель), процесс может упасть,
   не хотим пере-жевывать всё с начала.
7. Один поток, один запрос к Ollama одновременно — чтобы не убивать машину.

Использование
-------------
    python3 ingest_pipeline.py /path/to/books --kind book
    python3 ingest_pipeline.py /path/to/notes --kind note
    python3 ingest_pipeline.py /path/to/src   --kind code

    Флаги:
      --dry-run       не отправлять в ядро, только печатать извлечённое
      --reset         забыть чекпоинт и начать заново
      --model NAME    имя модели в Ollama (по умолчанию qwen2.5:3b)
      --chunk-size N  размер чанка в символах (по умолчанию 700 — маленький,
                       чтобы 3B модель не "плыла")
"""

import argparse
import ast
import hashlib
import json
import re
import sys
import time
from pathlib import Path

import requests

sys.path.append(str(Path(__file__).resolve().parent))
from runtime.ipc import IPCClient  # noqa: E402  (используем ваш существующий клиент)

# --------------------------------------------------------------------------- #
# Конфигурация
# --------------------------------------------------------------------------- #

OLLAMA_API = "http://localhost:11434/api/generate"
DEFAULT_MODEL = "qwen2.5:3b"
REQUEST_TIMEOUT = 180          # 3B модель на CPU может думать долго
SLEEP_BETWEEN_CALLS = 0.3      # даём машине отдышаться между запросами
MAX_RETRIES = 2

CHECKPOINT_PATH = Path(__file__).resolve().parent / ".ingest_checkpoint.json"

# --------------------------------------------------------------------------- #
# Промпты — ОДНА согласованная схема на весь конвейер.
# Формат nodes/edges 1-в-1 совпадает с тем, что парсит core/src/perception/perception.c
# (perceive_and_activate): id/label/danger/utility и source/target/relation.
# --------------------------------------------------------------------------- #

LITERAL_PROMPT = """Ты — предельно точный и безэмоциональный экстрактор фактов.
Читай ТОЛЬКО то, что явно написано в тексте. НИЧЕГО не додумывай и не обобщай.

Извлеки:
- "nodes": сущности, инструменты, техники, уязвимости, команды, концепции.
  Поля: id (snake_case, англ.), label (как в тексте), danger (0..1, насколько
  опасно/разрушительно), utility (0..1, насколько полезно как инструмент/приём).
- "edges": явные отношения между узлами из текста.
  Поля: source, target, relation (например CAUSES, USES, REQUIRES, EXPLOITS,
  PART_OF, LEADS_TO — заглавными буквами, англ.).

Если в тексте нет фактов — верни пустые списки. Не выдумывай.

Верни СТРОГО валидный JSON и ничего кроме него:
{{"nodes": [{{"id":"...", "label":"...", "danger":0.0, "utility":0.0}}],
  "edges": [{{"source":"...", "target":"...", "relation":"..."}}]}}

Текст:
\"\"\"{chunk}\"\"\"
"""

# Для личных заметок: короткие, туманные, могут значить сразу несколько вещей.
# Просим МОДЕЛЬ явно перечислить варианты толкования с уверенностью — это
# ложится в вашу модель как Hypothesis (не Fact), что архитектурно корректно.
INTERPRET_PROMPT = """Ты помогаешь восстановить смысл собственных черновых заметок
автора по программированию/пентесту. Заметка написана "для себя" и может быть
неполной, содержать сокращения или неочевидные ссылки на контекст.

Не пытайся угадать ЕДИНСТВЕННЫЙ правильный смысл. Вместо этого предложи от 1 до 3
РАЗНЫХ правдоподобных толкований того, что автор имел в виду и зачем это записал
(идея? баг? todo? наблюдение? план атаки? архитектурное решение?).

Для каждого толкования укажи confidence (0..1) — насколько ты уверен, что именно
это имел в виду автор.

Верни СТРОГО валидный JSON и ничего кроме него, в формате гипер-атомов:
{{"atoms": [
  {{"process": "MEANS", "args": ["<заметка_кратко>", "<толкование>"], "confidence": 0.6}}
]}}

Заметка:
\"\"\"{chunk}\"\"\"
"""

# --------------------------------------------------------------------------- #
# Чекпоинт: что уже обработано (по хэшу содержимого чанка), чтобы не повторять
# работу слабой модели заново после падения/прерывания.
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
        # Пишем после каждого чанка — обработка медленная, потеря прогресса
        # при падении процесса недопустима.
        self.path.write_text(json.dumps(list(self.done)))

    def reset(self):
        self.done = set()
        if self.path.exists():
            self.path.unlink()


def chunk_key(text: str, tag: str) -> str:
    return tag + ":" + hashlib.sha1(text.encode("utf-8", "ignore")).hexdigest()


# --------------------------------------------------------------------------- #
# Ollama: вызов + устойчивый парсинг JSON (модель иногда добавляет мусор
# вокруг JSON или обрезает вывод — это норма для 3B модели, нужно уметь чинить).
# --------------------------------------------------------------------------- #


def call_ollama(prompt: str, model: str) -> dict | None:
    payload = {"model": model, "prompt": prompt, "format": "json", "stream": False}
    for attempt in range(MAX_RETRIES + 1):
        try:
            resp = requests.post(OLLAMA_API, json=payload, timeout=REQUEST_TIMEOUT)
            resp.raise_for_status()
            raw = resp.json().get("response", "")
            parsed = _repair_and_parse_json(raw)
            if parsed is not None:
                return parsed
        except requests.exceptions.Timeout:
            print(f"  [ollama] timeout (попытка {attempt + 1}/{MAX_RETRIES + 1})", file=sys.stderr)
        except Exception as e:
            print(f"  [ollama] ошибка: {e}", file=sys.stderr)
        time.sleep(1.0)
    return None


def _repair_and_parse_json(raw: str) -> dict | None:
    raw = raw.strip()
    # снять возможные ```json ... ``` обёртки
    raw = re.sub(r"^```(json)?", "", raw).strip()
    raw = re.sub(r"```$", "", raw).strip()
    try:
        return json.loads(raw)
    except json.JSONDecodeError:
        pass
    # попытка вытащить первый {...} блок, если модель дописала лишний текст
    match = re.search(r"\{.*\}", raw, re.DOTALL)
    if match:
        try:
            return json.loads(match.group(0))
        except json.JSONDecodeError:
            return None
    return None


# --------------------------------------------------------------------------- #
# Чанкинг текста. Маленькие чанки + небольшой overlap, чтобы не рвать мысль
# ровно на границе, но и не перегружать контекст 3B модели.
# --------------------------------------------------------------------------- #


def chunk_text(text: str, size: int, overlap: int = 80):
    text = re.sub(r"\n{3,}", "\n\n", text).strip()
    if not text:
        return
    start = 0
    n = len(text)
    while start < n:
        end = min(start + size, n)
        # стараемся резать по границе предложения/строки, а не посреди слова
        if end < n:
            cut = text.rfind("\n", start, end)
            if cut == -1 or cut <= start + size // 2:
                cut = text.rfind(". ", start, end)
            if cut != -1 and cut > start + size // 3:
                end = cut + 1
        piece = text[start:end].strip()
        if len(piece) > 40:  # пропускаем совсем пустые обрывки
            yield piece
        start = end - overlap if end - overlap > start else end


# --------------------------------------------------------------------------- #
# Source Adapters
# --------------------------------------------------------------------------- #


def read_pdf(path: Path) -> str:
    import fitz  # PyMuPDF
    doc = fitz.open(path)
    return "\n".join(page.get_text() for page in doc)


def read_text_file(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="ignore")


def extract_code_structure(path: Path) -> dict:
    """
    Детерминированный (не-LLM) разбор кода: функции, классы, импорты, докстринги.
    Экономит вызовы LLM там, где смысл извлекается парсингом, а не пониманием.
    Пока полноценно поддержан Python (ast). Для C/H — простая эвристика.
    """
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
        # C / H и прочее: грубая эвристика по #include и сигнатурам функций
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
# Основной конвейер
# --------------------------------------------------------------------------- #


class Ingestor:
    def __init__(self, model: str, dry_run: bool, checkpoint: Checkpoint):
        self.model = model
        self.dry_run = dry_run
        self.checkpoint = checkpoint
        self.ipc = None
        if not dry_run:
            self.ipc = IPCClient()
            self.ipc.connect()

    def send_graph(self, payload: dict):
        if not payload.get("nodes") and not payload.get("edges") and not payload.get("atoms"):
            return
        if self.dry_run:
            print(json.dumps(payload, ensure_ascii=False, indent=2))
            return
        resp = self.ipc.command("learn", json.dumps(payload))
        ok = resp.get("payload", {})
        print(f"  -> learn: {ok}")

    def process_book(self, path: Path, chunk_size: int):
        print(f"[BOOK] {path.name}")
        text = read_pdf(path) if path.suffix.lower() == ".pdf" else read_text_file(path)
        for i, chunk in enumerate(chunk_text(text, chunk_size)):
            key = chunk_key(chunk, f"book:{path.name}")
            if self.checkpoint.is_done(key):
                continue
            print(f"  чанк {i}: {len(chunk)} симв.")
            result = call_ollama(LITERAL_PROMPT.format(chunk=chunk), self.model)
            if result:
                self.send_graph(result)
            self.checkpoint.mark_done(key)
            time.sleep(SLEEP_BETWEEN_CALLS)

    def process_note(self, path: Path, chunk_size: int):
        print(f"[NOTE] {path.name}")
        text = read_text_file(path)
        # заметки режем мельче книг — они и так плотные по смыслу
        for i, chunk in enumerate(chunk_text(text, min(chunk_size, 400))):
            lit_key = chunk_key(chunk, f"note-lit:{path.name}")
            int_key = chunk_key(chunk, f"note-int:{path.name}")

            if not self.checkpoint.is_done(lit_key):
                print(f"  чанк {i} (литерально)")
                result = call_ollama(LITERAL_PROMPT.format(chunk=chunk), self.model)
                if result:
                    self.send_graph(result)
                self.checkpoint.mark_done(lit_key)
                time.sleep(SLEEP_BETWEEN_CALLS)

            if not self.checkpoint.is_done(int_key):
                print(f"  чанк {i} (интерпретация)")
                result = call_ollama(INTERPRET_PROMPT.format(chunk=chunk), self.model)
                if result:
                    self.send_graph(result)
                self.checkpoint.mark_done(int_key)
                time.sleep(SLEEP_BETWEEN_CALLS)

    def process_code(self, path: Path):
        key = chunk_key(path.name + str(path.stat().st_mtime), f"code:{path}")
        if self.checkpoint.is_done(key):
            return
        print(f"[CODE] {path}")
        result = extract_code_structure(path)
        self.send_graph(result)
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
    ap.add_argument("path", type=str, help="Файл или папка с источниками")
    ap.add_argument("--kind", choices=["book", "note", "code"], required=True)
    ap.add_argument("--model", default=DEFAULT_MODEL)
    ap.add_argument("--chunk-size", type=int, default=700)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--reset", action="store_true")
    args = ap.parse_args()

    checkpoint = Checkpoint(CHECKPOINT_PATH)
    if args.reset:
        checkpoint.reset()

    root = Path(args.path)
    exts = EXT_BY_KIND[args.kind]
    files = [root] if root.is_file() else sorted(
        p for p in root.rglob("*") if p.is_file() and p.suffix.lower() in exts
    )
    print(f"Найдено файлов: {len(files)}")

    ingestor = Ingestor(model=args.model, dry_run=args.dry_run, checkpoint=checkpoint)
    try:
        for path in files:
            try:
                if args.kind == "book":
                    ingestor.process_book(path, args.chunk_size)
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
