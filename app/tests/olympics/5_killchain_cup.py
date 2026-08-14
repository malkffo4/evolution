#!/usr/bin/env python3
# app/tests/olympics/5_killchain_cup.py
"""AGI OLYMPICS: KILLCHAIN CUP — усвоение методологии без словарных
галлюцинаций (Level 3) + перенос на логи, которых не было в тексте (Level 4)."""
import sys
from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[2]
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

from core.manager import EvolutionManager
from knowledge.domain_prompts import KILLCHAIN_EXTRACTION_PROMPT, KILLCHAIN_ORDER, ingest_domain, validate_killchain_atoms
from knowledge.log_classifier import ingest_log_line, wait_for_hypothesis
from tools.knowledge_report import print_diff


def get_atoms(core, query: str) -> list:
    resp = core.retrieve(query)
    return resp.get("atoms", []) if isinstance(resp, dict) else []


def relation_exists(core, relation: str, a: str, b: str) -> bool:
    for q in (a, relation):
        for atom in get_atoms(core, q):
            if atom.get("process") == relation and atom.get("args") == [a, b]:
                return True
    return False


KILLCHAIN_TEXT = """
Cyber Kill Chain описывает этапы кибератаки. Reconnaissance предшествует
Weaponization. Weaponization предшествует Delivery. Delivery предшествует
Exploitation. Exploitation предшествует Installation. Installation
предшествует Command_and_Control. Command_and_Control предшествует
Actions_on_Objectives. Reconnaissance использует технику port_scan.
Delivery часто использует технику phishing_attachment. Command_and_Control
часто проявляется как исходящие DNS-маячки (dns_beaconing).
"""


def main():
    print("=" * 60)
    print("     AGI OLYMPICS: KILLCHAIN CUP")
    print("=" * 60 + "\n")

    manager = EvolutionManager()
    try:
        manager.initialize()
        core, llm = manager.core_client, manager.llm_client
        stats_before = core.get_stats()

        print("[KillChain Cup] Шаг 1: усвоение со строгим Critic-фильтром словаря...")
        result = ingest_domain(core._ipc, llm, KILLCHAIN_TEXT, KILLCHAIN_EXTRACTION_PROMPT,
                                source_tag="killchain_theory", domain="cybersec",
                                validator=validate_killchain_atoms)
        print(f"  -> принято={result['atoms_accepted']} отклонено_валидатором={result['atoms_rejected']}")

        print("\n[KillChain Cup] Шаг 2: проверка полноты цепочки PRECEDES...")
        missing = [(a, b) for a, b in zip(KILLCHAIN_ORDER, KILLCHAIN_ORDER[1:])
                   if not relation_exists(core, "PRECEDES", a, b)]
        assert not missing, f"Не извлечены переходы: {missing}"
        print("  [PASS] Все 6 переходов извлечены.")

        print("\n[KillChain Cup] Шаг 3: перенос на логи, не встречавшиеся как текст...")
        logs = [
            ("dns_case", "10.0.0.14 -> suspicious outbound DNS beacon every 60s after attachment opened"),
            ("scan_case", "SYN scan detected from external host across 1000 ports"),
        ]
        classified = 0
        for tag, line in logs:
            info = ingest_log_line(core, line, llm, session_tag=tag)
            hyp = wait_for_hypothesis(core, info["event_id"], timeout_sec=6.0)
            if hyp:
                classified += 1
                print(f"  [{tag}] '{info['technique']}' -> stage='{hyp['stage']}' "
                      f"confidence={hyp['confidence']:.2f}")
                assert hyp["confidence"] < 0.5
                assert hyp["process"] != "HAS_ALGORITHM"
            else:
                print(f"  [{tag}] '{info['technique']}' -> связи в KB не найдено (честный отказ)")

        assert classified >= 1, "Ни один лог не был связан с усвоенной онтологией"

        stats_after = core.get_stats()
        print_diff("Knowledge Atoms", int(stats_before.get("atoms_total", 0)), int(stats_after.get("atoms_total", 0)))
        print("\n" + "=" * 60 + "\n     KILLCHAIN CUP: SUCCESS\n" + "=" * 60)
    finally:
        manager.shutdown()


if __name__ == "__main__":
    main()
