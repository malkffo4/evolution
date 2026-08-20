import json
import sys
from pathlib import Path
from core.sdk import CoreClient, djb2_hash
from core.llm import LLMClient

IDOR_PROMPT = """Ты — фильтр безопасности.
Проанализируй URL. Есть ли в нем предсказуемый числовой или строковой идентификатор ресурса (например: id=123, /user/45, /doc/77)?
Не пиши пояснений. Верни строго JSON:
{{"is_candidate": true, "param_name": "имя_параметра"}}
URL: {url}
"""

def classify_endpoints(core: CoreClient, llm: LLMClient, urls: list[str]):
    for url in urls:
        raw_resp = llm.query(IDOR_PROMPT.format(url=url), json_mode=True)
        try:
            res = json.loads(raw_resp)
            if res.get("is_candidate"):
                # Создаем гипотезу в LMDB с низким доверием
                endpoint_id = f"endpoint_{djb2_hash(url)}"
                core.learn({"atoms": [
                    {
                        "id": endpoint_id,
                        "process": "OBSERVED_EVENT",
                        "args": [endpoint_id, "URL"],
                        "properties": {"url": url, "param": res["param_name"]}
                    },
                    {
                        "process": "HYPOTHESIS",
                        "args": [endpoint_id, "idor_candidate"],
                        "truth": {"mean": 0.3, "confidence": 0.5}, # Изначально не доверяем LLM
                        "attention": {"sti": 0.8, "lti": 0.1},
                        "enqueue": "cybersec::idor_check" # Отправляем в очередь на проверку
                    }
                ]})
                print(f"[+] Гипотеза создана: {url}")
        except json.JSONDecodeError:
            continue

if __name__ == "__main__":
    core = CoreClient().connect()
    llm = LLMClient()
    # Пример входа от краулера
    classify_endpoints(core, llm, ["https://api.example.com/v1/profile?user_id=105"])
