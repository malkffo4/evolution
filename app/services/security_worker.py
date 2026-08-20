import time
import httpx
import json
import uuid
from core.sdk import CoreClient, djb2_hash

QUEUE_NAME = "cybersec::idor_check"
QUEUE_HASH = djb2_hash(QUEUE_NAME)
ALGO_HASH = djb2_hash("CheckIdorAlgo")

def check_idor(url: str, param: str) -> float:
    """Детерминированная проверка IDOR без LLM."""
    client = httpx.Client(verify=False, timeout=5.0)
    try:
        # Базовый запрос (имитация токена пользователя A)
        headers_a = {"Authorization": "Bearer USER_A_TOKEN"}
        resp_a = client.get(url, headers=headers_a)

        if resp_a.status_code != 200:
            return 0.0 # Эндпоинт мертв

        # Тестовый запрос (Меняем ID ресурса +1, токен тот же)
        # В реальной жизни тут регулярка для замены ID в URL
        test_url = url.replace(f"{param}=105", f"{param}=106")
        resp_b = client.get(test_url, headers=headers_a)

        # Если статус 200, и тело не равно ошибке прав (простой дифф)
        if resp_b.status_code == 200 and len(resp_b.content) > 50 and resp_a.content != resp_b.content:
            if b"unauthorized" not in resp_b.content.lower():
                return 1.0 # Уязвимость подтверждена

    except Exception as e:
        print(f"[!] Ошибка HTTP: {e}")
    finally:
        client.close()

    return 0.0 # Провал гипотезы

def run_worker():
    core = CoreClient().connect()
    print(f"[*] Security Worker запущен. Ожидание очереди: {QUEUE_NAME}")

    while True:
        # 1. Забираем задачу из C-ядра
        # В реальном SDK тут будет обертка над OP_QUEUE_POP
        payload = {"op": "OP_QUEUE_POP", "regs": {"0": QUEUE_HASH}, "args": [0, 1, 2, 0, 0, 0]}
        resp = core._command("execute_op", json.dumps(payload))

        target_id = resp.get("reported_regs", {}).get("1", 0)

        if target_id == 0:
            time.sleep(2.0)
            continue

        print(f"[*] Получена цель: {target_id}")

        # В реальности здесь нужно дернуть get_property для target_id, чтобы получить URL
        # Для примера хардкодим:
        url = "https://api.example.com/v1/profile?user_id=105"
        param = "user_id"

        # 2. Детерминированное исполнение
        outcome = check_idor(url, param)
        print(f"[*] Результат проверки: Outcome = {outcome}")

        # 3. Фиксация эпизода в LMDB для срабатывания UCB1 и credit_assignment
        episode_id = f"ep_{uuid.uuid4().hex[:8]}"
        core.learn({"atoms": [
            {
                "id": episode_id,
                "process": "EPISODE",
                "kind": "episode",
                "args": [target_id, ALGO_HASH],
                "properties": {"outcome": outcome, "wall_time": int(time.time())}
            }
        ]})
        # Теперь C-ядро само поднимет или опустит confidence гипотезы через score_propagate_credit

if __name__ == "__main__":
    run_worker()
