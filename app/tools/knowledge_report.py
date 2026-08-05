#!/usr/bin/env python3
# app/tools/knowledge_report.py
"""
Снимает физические метрики (Knowledge Report) с C-ядра.
Используется для Black-Box оценки прогресса: сравниваем состояние базы
до загрузки книги/опыта и после.
Соответствует идеологии AGI Olympics (docs/17_Evaluation.md).
"""

import argparse
import json
import sys
from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[1]
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

from core.sdk import CoreClient

def print_diff(label: str, before: int, after: int):
    diff = after - before
    sign = "+" if diff >= 0 else ""
    color = "\033[92m" if diff > 0 else "\033[0m" # Green if positive
    reset = "\033[0m"

    print(f"{label}:")
    print(f"  before: {before}")
    print(f"  after:  {after}")
    print(f"  delta:  {color}{sign}{diff}{reset}\n")

def main():
    ap = argparse.ArgumentParser(description="KOSMOS Knowledge Report Generator")
    ap.add_argument("--baseline", type=Path, help="Путь к JSON файлу с предыдущими замерами")
    ap.add_argument("--save", type=Path, help="Сохранить текущие метрики как новый baseline.json")
    args = ap.parse_args()

    core = CoreClient()
    try:
        core.connect()
        stats = core.get_stats()
    except Exception as e:
        print(f"[ERROR] Не удалось подключиться к ядру: {e}", file=sys.stderr)
        sys.exit(1)
    finally:
        core.close()

    baseline = {}
    if args.baseline and args.baseline.exists():
        try:
            baseline = json.loads(args.baseline.read_text(encoding="utf-8"))
        except Exception as e:
            print(f"[WARN] Ошибка чтения baseline: {e}", file=sys.stderr)

    print("\n================= KNOWLEDGE REPORT =================")
    print("System: KOSMOS NeuroCore\n")

    print_diff("Knowledge Atoms (Concepts, Facts, Rules)",
               int(baseline.get("atoms_total", 0)), int(stats.get("atoms_total", 0)))

    print_diff("Causal Links (Reasoning depth, idx_causal)",
               int(baseline.get("causal_links", 0)), int(stats.get("causal_links", 0)))

    print_diff("Vector Groundings (Embodied semantics)",
               int(baseline.get("vectors_total", 0)), int(stats.get("vectors_total", 0)))

    print_diff("Life Episodes (Experience accumulated)",
               int(baseline.get("episodes_total", 0)), int(stats.get("episodes_total", 0)))

    print("====================================================\n")

    if args.save:
        args.save.write_text(json.dumps(stats, indent=2), encoding="utf-8")
        print(f"[OK] Текущий baseline сохранен в {args.save}")

if __name__ == "__main__":
    main()
