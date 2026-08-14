#!/usr/bin/env python3
# app/tools/knowledge_validator.py
"""
Garbage Collector / Validator: защита LMDB от галлюцинаций LLM.

Использует новый bounded-эндпоинт "audit_atoms" (курсорная пагинация по
db.graph.hyper.atoms, до AUDIT_BATCH_SIZE=500 атомов за вызов) и "mark_flaw"
(создаёт HAS_FLAW(atom, GarbageCandidate) с корректной REF-ссылкой).
Ничего не удаляется напрямую — только понижение sti, чтобы decay.c
архивировал такие атомы быстрее (см. docs/01_Principles.md, Principle 11).
"""
import argparse
import json
import sys
from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[1]
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

from core.ipc import IPCClient

MARK_BATCH_SIZE = 64  # совпадает с FlawJob.ids[64] на стороне C


def scan_cold_atoms(ipc: IPCClient, sti_th: float, lti_th: float) -> list:
    resume_id, collected, page = 0, [], 0
    while True:
        page += 1
        resp = ipc.request("audit_atoms", {
            "resume_id": resume_id, "sti_threshold": sti_th, "lti_threshold": lti_th,
        })
        payload = resp.get("payload", {})
        if isinstance(payload, str):
            payload = json.loads(payload) if payload else {}
        if "error" in payload:
            print(f"[validator] audit_atoms error: {payload['error']}", file=sys.stderr)
            break

        atoms = payload.get("atoms", [])
        collected.extend(atoms)
        print(f"  страница {page}: scanned={payload.get('scanned', 0)} "
              f"кандидатов={len(atoms)} всего={len(collected)}")

        if not payload.get("has_more"):
            break
        resume_id = payload.get("next_resume_id", 0)
        if resume_id == 0:
            break
    return collected


def classify(atom: dict) -> str:
    if not atom.get("has_ref_args", True):
        return "orphan"

    if atom.get("process") == "STAGE_HYPOTHESIS" and atom.get("truth_confidence", 0) < 0.5:
        return "needs_human_review"

    if atom["sti"] < 0.02 and atom["lti"] < 0.02 and atom["truth_confidence"] < 0.2:
        return "stale_low_confidence"
    return "cold"


def main():
    ap = argparse.ArgumentParser(description="NeuroCore Knowledge Validator / GC")
    ap.add_argument("--sti-threshold", type=float, default=0.10)
    ap.add_argument("--lti-threshold", type=float, default=0.10)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    ipc = IPCClient()
    ipc.connect()
    assert ipc.ping(), "Core not responding"

    print(f"[validator] скан атомов sti<{args.sti_threshold} lti<{args.lti_threshold} ...")
    candidates = scan_cold_atoms(ipc, args.sti_threshold, args.lti_threshold)

    if not candidates:
        print("[validator] мусорных кандидатов не найдено.")
        ipc.close()
        return

    buckets = {}
    for a in candidates:
        buckets.setdefault(classify(a), []).append(a)

    print(f"\n[validator] найдено {len(candidates)} кандидат(ов):")
    for kind, items in buckets.items():
        print(f"  {kind}: {len(items)}")

    if args.dry_run:
        print("\n[validator] --dry-run: ничего не помечаем.")
        ipc.close()
        return

    ids = [int(a["id"]) for a in candidates]
    marked = 0
    for i in range(0, len(ids), MARK_BATCH_SIZE):
        batch = ids[i:i + MARK_BATCH_SIZE]
        resp = ipc.command("mark_flaw", json.dumps({"atom_ids": batch}))
        payload = resp.get("payload", {})
        if isinstance(payload, str):
            payload = json.loads(payload) if payload else {}
        if "error" in payload:
            print(f"  batch {i // MARK_BATCH_SIZE}: {payload['error']}", file=sys.stderr)
            continue
        marked += payload.get("flagged", 0)

    print(f"\n[validator] помечено {marked}/{len(ids)} как HAS_FLAW(atom, GarbageCandidate).")
    print("[validator] Будут архивированы ближайшим subconscious_decay_cycle() (тик 10с).")
    ipc.close()


if __name__ == "__main__":
    main()
