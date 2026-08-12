#!/usr/bin/env python3
# app/tests/test_semantic_fallback.py

import sys
import json
from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[1]
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

from core.ipc import IPCClient
from knowledge.domain_ns import namespace_atom_args
from knowledge.embeddings import augment_atoms_with_entity_embeddings
from knowledge.rag_pipeline import NeuroSymbolicRAG

def main():
    ipc = IPCClient()
    ipc.connect()
    assert ipc.ping(), "Ядро KOSMOS не отвечает!"

    # 1. Готовим сырые атомы (как будто их только что выдала LLM)
    raw_atoms = [
        {
            "process": "CAUSES",
            "kind": "relation",
            "args": ["strcpy", "buffer overflow"],
            "truth": {"mean": 1.0, "confidence": 0.9}
        }
    ]

    # 2. Навешиваем неймспейс домена 'cybersec'
    for a in raw_atoms:
        namespace_atom_args(a, "cybersec")

    # 3. Генерируем 128-float векторы (hashing trick) для новых сущностей
    atoms_to_learn = augment_atoms_with_entity_embeddings(raw_atoms)

    # Учим ядро
    resp = ipc.command("learn", json.dumps({"atoms": atoms_to_learn}))
    if resp.get("name") == "error":
        print(f"❌ Ошибка загрузки: {resp}")
        sys.exit(1)

    print("[+] Факты с неймспейсом 'cybersec::' и векторами загружены в ядро.")

    # 4. Проверяем семантический поиск
    rag = NeuroSymbolicRAG(ipc)
    print("\n--- ТЕСТ: Семантический поиск ---")

    # Ищем с опечаткой и без неймспейса
    query = "buffer over flow"
    print(f"Запрос от пользователя (RAG): '{query}'\n")

    context = rag.get_graph_context(query)

    if "[Семантический поиск]" in context:
        print("   УСПЕХ: Точный хэш промахнулся, но сработал семантический фоллбэк через LSH/Cosine Similarity!")
        print("Полученный контекст из C-ядра:")
        print("-" * 40)
        print(context)
        print("-" * 40)
    elif context:
        print("   Найдено точное совпадение (возможно, тестируешь на старой базе).")
        print(context)
    else:
        print("   ОШИБКА: Семантический поиск ничего не нашел.")

    ipc.close()

if __name__ == "__main__":
    main()
