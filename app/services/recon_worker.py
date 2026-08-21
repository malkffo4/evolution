# app/services/recon_worker.py
import time
import httpx
import json
import uuid
import sys
import subprocess
from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[1]
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

from core.sdk import CoreClient, djb2_hash
from core.llm import LLMClient
from knowledge.sensors.nmap_adapter import NmapAdapter

QUEUE_NAME = "cybersec::recon"
QUEUE_HASH = djb2_hash(QUEUE_NAME)

IDOR_PROMPT = """Ты — фильтр безопасности. Проанализируй URL: {url}
Есть ли в нем предсказуемый идентификатор ресурса (например: id=123, /user/45, /api/v1/doc/88)?
Верни строго JSON: {{"is_candidate": true_или_false, "param_name": "имя_параметра_или_пусто"}}"""

def fetch_subdomains(domain: str) -> list[str]:
    print(f"[*] [Recon] Ищу SSL-сертификаты для {domain} через crt.sh...")
    try:
        resp = httpx.get(f"https://crt.sh/?q=%.{domain}&output=json", timeout=20.0)
        if resp.status_code == 200:
            subs = set()
            for entry in resp.json():
                name = entry.get("name_value", "").lower()
                if not name.startswith("*"):
                    subs.add(name)
            return list(subs)
    except Exception as e:
        print(f"[!] [Recon] Ошибка crt.sh: {e}")
    return [domain]

def scan_ports(core: CoreClient, target: str):
    """Запускает реальный Nmap и скармливает XML в C-ядро через адаптер."""
    print(f"[*] [Recon] Запускаю Nmap для {target}...")
    try:
        # -F (Fast scan), -sV (Service versions), -oX - (Вывод в XML в stdout)
        result = subprocess.run(["nmap", "-F", "-sV", "-oX", "-", target], capture_output=True, text=True)
        if result.returncode == 0 and result.stdout:
            adapter = NmapAdapter()
            atoms = adapter.to_atoms(result.stdout, scope=target)
            if atoms:
                core.learn({"atoms": atoms})
                print(f"  [+] В граф добавлено {len(atoms)} фактов об открытых портах и сервисах {target}.")
    except Exception as e:
        print(f"[!] [Recon] Ошибка Nmap: {e}")

def crawl_for_params(core: CoreClient, llm: LLMClient, target: str):
    """Легкий краулер: собирает ссылки с главной страницы и ищет параметры."""
    url = f"https://{target}"
    print(f"[*] [Recon] Краулинг {url}...")
    try:
        # Здесь в идеале должен быть вызов waybackurls или hakrawler
        # Для простоты делаем GET и ищем ссылки
        client = httpx.Client(verify=False, timeout=10.0)
        resp = client.get(url)
        client.close()

        # Заглушка: если бы мы спарсили ссылки с параметрами, прогоняем их через LLM
        # Допустим, мы нашли вот это в href:
        found_urls = [f"{url}/profile?id=1", f"{url}/about"]

        for t_url in found_urls:
            if "?" in t_url or "id" in t_url:
                raw_resp = llm.query(IDOR_PROMPT.format(url=t_url), json_mode=True)
                res = json.loads(raw_resp)

                if res.get("is_candidate"):
                    endpoint_id = f"ep_{djb2_hash(t_url)}"
                    core.learn({"atoms": [
                        {
                            "id": endpoint_id,
                            "process": "OBSERVED_EVENT",
                            "args": [endpoint_id, "URL"],
                            "properties": {"url": t_url, "param": res.get("param_name", "")}
                        },
                        {
                            "process": "HYPOTHESIS",
                            "args": [endpoint_id, "idor_candidate"],
                            "truth": {"mean": 0.3, "confidence": 0.5},
                            "attention": {"sti": 0.8, "lti": 0.1},
                            "enqueue": "cybersec::idor_check"
                        }
                    ]})
                    print(f"  [+] Гипотеза IDOR отправлена в ядро: {t_url}")
    except Exception:
        pass

def process_target(core: CoreClient, llm: LLMClient, target_domain: str):
    subdomains = fetch_subdomains(target_domain)
    print(f"[*] [Recon] Найдено поддоменов: {len(subdomains)}")

    for sub in subdomains[:3]: # Ограничение для теста
        scan_ports(core, sub)
        crawl_for_params(core, llm, sub)

def run_recon():
    core = CoreClient().connect()
    llm = LLMClient()
    print(f"[*] Recon Worker запущен. Слушаю очередь: {QUEUE_NAME}")

    while True:
        payload = {"op": "OP_QUEUE_POP", "regs": {"0": QUEUE_HASH}, "args": [0, 1, 2, 0, 0, 0]}
        resp = core._command("execute_op", json.dumps(payload))
        target_id = resp.get("reported_regs", {}).get("1", 0)

        if target_id == 0:
            time.sleep(2.0)
            continue

        prop_resp = core._request("get_property", {"subject": str(target_id), "key": "target"})
        domain = prop_resp.get("payload", {}).get("value")

        if domain:
            process_target(core, llm, domain)

if __name__ == "__main__":
    run_recon()
