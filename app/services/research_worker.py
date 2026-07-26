#!/usr/bin/env python3
# app/services/research_worker.py
import json
import sys
import time
from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[1]   # .../app
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

from core.ipc import IPCClient
from core.llm import LLMClient
from knowledge.prompts import EXTRACTION_PROMPT

POLL_INTERVAL_SEC = 3
MAX_WEB_RESULTS = 5

class ResearchWorker:
    """Замыкает цикл: pending_task (из VM_NOT_FOUND в vm_op_evaluate_goals)
    -> веб-поиск -> LLM-экстракция в единую схему -> learn в ядро.
    После learn planner на следующем тике MainLoop может найти новый HAS_ALGORITHM/факт,
    и cooldown цели (set_goal_cooldown) естественным образом снимется по истечении времени."""

    def __init__(self, provider="ollama", model=None):
        self.ipc = IPCClient()
        self.ipc.connect()
        self.llm = LLMClient(provider=provider, model=model)

    def _web_search(self, query: str) -> str:
        try:
            from duckduckgo_search import DDGS
            with DDGS() as ddgs:
                results = list(ddgs.text(query, max_results=MAX_WEB_RESULTS))
            return "\n".join(f"{r.get('title','')}: {r.get('body','')}" for r in results)
        except Exception as e:
            print(f"[ResearchWorker] web search failed: {e}", file=sys.stderr)
            return ""

    def _wikipedia(self, query: str) -> str:
        try:
            import wikipediaapi
            wiki = wikipediaapi.Wikipedia("NeuroCore/1.0", "ru")
            page = wiki.page(query)
            return page.summary[:2000] if page.exists() else ""
        except Exception:
            return ""

    def process_task(self, task: dict):
        query = task["query"]
        node_id = task["node_id"]
        print(f"[ResearchWorker] researching '{query}' (node_id={node_id})")

        context = "\n\n".join(filter(None, [self._wikipedia(query), self._web_search(query)]))
        if not context.strip():
            print(f"[ResearchWorker] no context found for '{query}'")
            return

        prompt = EXTRACTION_PROMPT.format(chunk=context[:3000])
        raw = self.llm.query(prompt, json_mode=True)
        try:
            graph = json.loads(raw)
        except json.JSONDecodeError:
            print(f"[ResearchWorker] LLM returned invalid JSON for '{query}'", file=sys.stderr)
            return

        if not graph.get("nodes") and not graph.get("edges"):
            return

        resp = self.ipc.command("learn", json.dumps(graph))
        print(f"[ResearchWorker] learned {len(graph.get('nodes', []))} nodes, "
              f"{len(graph.get('edges', []))} edges -> {resp.get('payload')}")

    def run(self):
        print("[ResearchWorker] started, polling for pending research tasks...")
        while True:
            try:
                resp = self.ipc.request("get_res_tasks")
                tasks = resp.get("payload", [])
                if isinstance(tasks, str):
                    tasks = json.loads(tasks) if tasks else []
                for task in tasks:
                    self.process_task(task)
            except Exception as e:
                print(f"[ResearchWorker] loop error: {e}", file=sys.stderr)
            time.sleep(POLL_INTERVAL_SEC)


if __name__ == "__main__":
    ResearchWorker().run()
