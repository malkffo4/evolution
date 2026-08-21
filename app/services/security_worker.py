# app/services/security_worker.py
import time
import httpx
import json
import uuid
import sys
from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[1]
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

from core.sdk import CoreClient, djb2_hash

QUEUE_NAME = "cybersec::idor_check"
QUEUE_HASH = djb2_hash(QUEUE_NAME)
ALGO_HASH = djb2_hash("CheckIdorAlgo")

def check_idor(url: str, param: str) -> float:
    print(f"[*] [Attack] Тестирую {url} на IDOR...")
    client = httpx.Client(verify=False, timeout=5.0)
    try:
        headers = {"Authorization": "Bearer TEST_TOKEN"}
        resp_a = client.get(url, headers=headers)
        if resp_a.status_code != 200:
            return 0.0

        # Меняем ID для проверки на чужой профиль
        test_url = url.replace(f"{param}=105", f"{param}=106")
        resp_b = client.get(test_url, headers=headers)

        if resp_b.status_code == 200 and len(resp_b.content) > 50 and resp_a.content != resp_b.content:
            if b"unauthorized" not in resp_b.content.lower():
                print("  [!!!] Уязвимость ПОДТВЕРЖДЕНА!")
                return 1.0
    except Exception:
        pass
    finally:
        client.close()
    return 0.0

def run_security():
    core = CoreClient().connect()
    print(f"[*] Security Worker запущен. Слушаю очередь: {QUEUE_NAME}")

    while True:
        payload = {"op": "OP_QUEUE_POP", "regs": {"0": QUEUE_HASH}, "args": [0, 1, 2, 0, 0, 0]}
        resp = core._command("execute_op", json.dumps(payload))
        target_id = resp.get("reported_regs", {}).get("1", 0)

        if target_id == 0:
            time.sleep(2.0)
            continue

        url_resp = core._request("get_property", {"subject": str(target_id), "key": "url"})
        param_resp = core._request("get_property", {"subject": str(target_id), "key": "param"})
        url = url_resp.get("payload", {}).get("value")
        param = param_resp.get("payload", {}).get("value")

        if url and param:
            outcome = check_idor(url, param)
            episode_id = f"ep_{uuid.uuid4().hex[:8]}"
            core.learn({"atoms": [{
                "id": episode_id, "process": "EPISODE", "kind": "episode",
                "args": [str(target_id), str(ALGO_HASH)],
                "properties": {"outcome": outcome, "wall_time": int(time.time())}
            }]})
            print(f"[*] [Attack] Эпизод отправлен в ядро (outcome={outcome}). Score обновлен.")

if __name__ == "__main__":
    run_security()
