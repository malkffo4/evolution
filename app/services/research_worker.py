# app/services/research_worker.py
import json, time
from core.ipc import IPCClient
from core.llm import LLMClient
from services.web_surfer import WebSurfer   # см. пункт 5

class ResearchWorker:
    def __init__(self):
        self.ipc = IPCClient()
        self.ipc.connect()
        self.llm = LLMClient()
        self.surfer = WebSurfer(self.ipc)

    def run(self):
        while True:
            resp = self.ipc.request("get_research_tasks")
            tasks = resp.get("payload", [])
            if isinstance(tasks, str):
                tasks = json.loads(tasks)
            for task in tasks:
                query = task["query"]
                # 1. Поиск в интернете
                content = self.surfer.search_and_extract(query)
                if content:
                    # 2. LLM извлекает факты
                    self.llm.query(f"Извлеки факты из: {content}", json_mode=True)
                    # 3. Отправить результат в ядро через learn
                    # ...
            time.sleep(2)
