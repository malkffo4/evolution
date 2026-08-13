# app/environments/web.py
import httpx
import hashlib
from typing import Dict, Any
from bs4 import BeautifulSoup

from environments.base import EnvironmentRuntime

class HttpReconEnvironment(EnvironmentRuntime):
    """
    Реализация Capability "FetchRawHtml".
    Быстрый HTTP-клиент для Reconnaissance. Не исполняет JS.
    """
    def __init__(self):
        # Используем современный HTTP/2 клиент
        self.client = httpx.Client(
            http2=True,
            verify=False, # Для Bug Bounty часто нужны self-signed сертификаты
            timeout=10.0,
            follow_redirects=True,
            headers={"User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"}
        )
        self.current_url = ""
        self.current_html = ""
        self.current_headers = {}

    def observe(self) -> Dict[str, Any]:
        """Возвращает сырой HTML и извлеченные ссылки для паукования."""
        affordances = {}

        if self.current_html:
            soup = BeautifulSoup(self.current_html, "html.parser")

            # В HTTP режиме нас интересуют только ссылки для краулинга
            for idx, a_tag in enumerate(soup.find_all("a", href=True)):
                href = a_tag["href"]
                el_id = f"link_{hashlib.md5(href.encode()).hexdigest()[:8]}"
                affordances[el_id] = {
                    "type": "navigate",
                    "label": a_tag.get_text(strip=True)[:50],
                    "url": href # Сохраняем URL прямо в аффорданс
                }

        return {
            "state": {
                "url": self.current_url,
                "headers": dict(self.current_headers),
                "content": self.current_html[:5000] # Обрезаем для C-ядра
            },
            "affordances": affordances
        }

    def act(self, affordance_id: str, params: Dict[str, Any] = None) -> Dict[str, Any]:
        params = params or {}

        if affordance_id == "navigate":
            url = params.get("url")
            if not url: return {"success": False, "error": "URL missing"}

            try:
                response = self.client.get(url)
                self.current_url = str(response.url)
                self.current_html = response.text
                self.current_headers = response.headers
                return {"success": True, "observation": self.observe()}
            except Exception as e:
                return {"success": False, "error": str(e)}

        return {"success": False, "error": f"Action {affordance_id} not supported in HTTP backend"}

    def close(self):
        self.client.close()
