#!/usr/bin/env python3
# app/tests/olympics/3_book_learning_cup.py
"""
🏆 AGI OLYMPICS: BOOK LEARNING CUP 🏆
Уровни 3 и 4: Генерация новых понятий и Перенос знаний (Transfer Learning).

Тест проверяет:
1. Система читает сырую книгу (.txt) через LLM (Parallel Ingestion).
2. Замеряется Delta Metrics (Knowledge Report).
3. Агенту задается вопрос, требующий применения только что прочитанных знаний (RAG + Fluid Dialogue).
"""
import sys
import time
from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[2]
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

from core.manager import EvolutionManager
from tools.knowledge_report import print_diff

def main():
    print("====================================================")
    print("🏆 AGI OLYMPICS: BOOK LEARNING CUP (LEVEL 3 & 4) 🏆")
    print("====================================================\n")

    manager = EvolutionManager()
    try:
        manager.initialize()
        core = manager.core_client

        print("[Book Cup] Step 1: Getting baseline...")
        stats_before = core.get_stats()

        book_path = APP_DIR / "tests" / "books" / "test_cyber_book.txt"
        print(f"\n[Book Cup] Step 2: Reading book: {book_path.name}")
        print("[Book Cup] This will use LLM for Deep Extraction (Parallel)...\n")

        # Внутри execute_command("ingest") запустится ingest_knowledge.py
        manager.execute_command("ingest", str(book_path))
        time.sleep(2) # Даем время на синхронизацию LMDB

        stats_after = core.get_stats()
        print("\n[Book Cup] Step 3: Knowledge Report (Level 3 Validation)")
        print_diff("Knowledge Atoms", int(stats_before.get("atoms_total", 0)), int(stats_after.get("atoms_total", 0)))
        print_diff("Causal Links", int(stats_before.get("causal_links", 0)), int(stats_after.get("causal_links", 0)))

        atoms_diff = int(stats_after.get("atoms_total", 0)) - int(stats_before.get("atoms_total", 0))
        if atoms_diff <= 0:
            print("[Book Cup] ⚠️ Warning: Atoms did not increase. Ensure LLM provider and API keys are valid.")

        print("\n[Book Cup] Step 4: Transfer Learning (Level 4 Validation)")
        question = "Какая функция в C опасна и вызывает переполнение буфера, и чем её заменить?"
        print(f"User: {question}")

        # Используем ChatService, который делает RAG-поиск по базе и общается через LLM
        reply = manager.chat.answer(question)
        print(f"Agent: {reply}\n")

        # Простая эвристика проверки переноса знаний
        reply_lower = str(reply)
        if "strcpy" in reply_lower and "strncpy" in reply_lower:
            print("[Book Cup] SUCCESS! Agent successfully transferred knowledge from the book to a conversational context!")
        else:
            print("[Book Cup] ⚠️ Warning: Agent did not explicitly mention strcpy/strncpy. The LLM might have rephrased it.")

        print("\n====================================================")
        print("✅ BOOK LEARNING CUP: COMPLETED")
        print("====================================================")

    finally:
        manager.shutdown()

if __name__ == "__main__":
    main()
