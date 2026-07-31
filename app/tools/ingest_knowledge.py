#!/usr/bin/env python3
# app/tools/ingest_knowledge.py
"""
Knowledge Ingestion Pipeline (RFC-0002, TODO Priority 2).

Text -> Chunking -> LLM extraction (EXTRACTION_PROMPT) -> IPC "learn" ->
perceive_hyper_json() -> HyperMemory. Использует тот же формат атомов
и тот же IPC-путь, что и app/services/research_worker.py — никаких
новых C-структур или таблиц LMDB.
"""
import argparse
import json
import re
import sys
import time
from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[1]
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

from core.ipc import IPCClient
from core.llm import LLMClient
from knowledge.prompts import EXTRACTION_PROMPT

CHUNK_SIZE_CHARS = 2800   # запас под EXTRACTION_PROMPT.format(chunk=text[:3000])
CHUNK_OVERLAP = 200       # не рвём сущность/предложение на границе чанка
MAX_RETRIES = 2


def chunk_text(text: str, size: int = CHUNK_SIZE_CHARS, overlap: int = CHUNK_OVERLAP) -> list:
    """Режем по границам предложений, а не посередине слова."""
    sentences = re.split(r'(?<=[.!?])\s+', text.strip())
    chunks, current = [], ""
    for s in sentences:
        if len(current) + len(s) + 1 > size and current:
            chunks.append(current.strip())
            current = current[-overlap:] + " " + s
        else:
            current = (current + " " + s).strip()
    if current.strip():
        chunks.append(current.strip())
    return chunks


def _parse_llm_json(raw: str):
    if not raw:
        return None
    raw = re.sub(r"^```(json)?", "", raw.strip()).strip()
    raw = re.sub(r"```$", "", raw).strip()
    try:
        return json.loads(raw)
    except json.JSONDecodeError:
        pass
    m = re.search(r"\{.*\}", raw, re.DOTALL)
    if m:
        try:
            return json.loads(m.group(0))
        except json.JSONDecodeError:
            return None
    return None


def extract_atoms(llm: LLMClient, chunk: str) -> list:
    for attempt in range(MAX_RETRIES + 1):
        raw = llm.query(EXTRACTION_PROMPT.format(chunk=chunk), json_mode=True)
        parsed = _parse_llm_json(raw)
        if parsed is not None and isinstance(parsed.get("atoms"), list):
            return parsed["atoms"]
        print(f"  [ingest] невалидный JSON от LLM (попытка {attempt+1}/{MAX_RETRIES+1}), retry...",
              file=sys.stderr)
    return []


def ingest_file(ipc: IPCClient, llm: LLMClient, path: Path, source_tag: str = None) -> dict:
    text = path.read_text(encoding="utf-8", errors="replace")
    chunks = chunk_text(text)
    source_tag = source_tag or path.name
    total_atoms = 0

    print(f"[ingest] '{path.name}': {len(chunks)} чанк(ов), ~{len(text)} символов")

    for i, chunk in enumerate(chunks, 1):
        atoms = extract_atoms(llm, chunk)
        if not atoms:
            print(f"  [{i}/{len(chunks)}] атомов не извлечено, пропуск")
            continue

        # Provenance (docs/03_Knowledge.md: "Evidence") — если LLM не указала
        # context сама, проставляем источник, чтобы знание не было "ничьим".
        for a in atoms:
            a.setdefault("context", source_tag)

        resp = ipc.command("learn", json.dumps({"atoms": atoms}))
        if resp.get("name") == "error":
            print(f"  [{i}/{len(chunks)}] learn failed: {resp.get('payload')}", file=sys.stderr)
            continue

        total_atoms += len(atoms)
        print(f"  [{i}/{len(chunks)}] +{len(atoms)} атомов (всего: {total_atoms})")
        time.sleep(0.05)  # не забиваем IPC-очередь одномоментно

    return {"file": str(path), "chunks": len(chunks), "atoms": total_atoms}


def main():
    ap = argparse.ArgumentParser(description="NeuroCore Knowledge Ingestion")
    ap.add_argument("path", type=Path, help=".txt или .md файл")
    ap.add_argument("--provider", default="ollama", choices=["ollama", "openai", "gemini"])
    ap.add_argument("--source", default=None)
    args = ap.parse_args()

    if not args.path.exists():
        sys.exit(f"[ingest] файл не найден: {args.path}")
    if args.path.suffix.lower() not in (".txt", ".md"):
        sys.exit(f"[ingest] неподдерживаемое расширение: {args.path.suffix}")

    ipc = IPCClient()
    ipc.connect()
    assert ipc.ping(), "Core not responding"
    llm = LLMClient(provider=args.provider)

    result = ingest_file(ipc, llm, args.path, args.source)
    print(f"\n[ingest] ГОТОВО: {result['atoms']} атомов из {result['chunks']} чанков -> HyperMemory")
    ipc.close()


if __name__ == "__main__":
    main()