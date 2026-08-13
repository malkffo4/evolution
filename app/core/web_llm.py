# app/core/web_llm.py
import os
import sys
import time
from environments.web import WebEnvironment

class WebLLMClient:
    """
    Абстрактный клиент для веб-версий LLM.
    Работает ИСКЛЮЧИТЕЛЬНО через семантический интерфейс WebEnvironment.
    """
    _instance = None

    def __new__(cls, *args, **kwargs):
        if cls._instance is None:
            cls._instance = super().__new__(cls)
            cls._instance._initialized = False
        return cls._instance

    def __init__(self, provider="web_deepseek", headless=False):
        if self._initialized:
            self.provider = provider
            return

        self.provider = provider
        self.headless = headless
        user_data_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".browser_session"))

        self.last_request_time = 0
        self.min_delay_sec = 12.0

        print(f"[WebLLM] Запускаю WebEnvironment для {provider}...")

        # ИСПОЛЬЗУЕМ НОВУЮ АБСТРАКЦИЮ
        self.env = WebEnvironment(headless=self.headless, user_data_dir=user_data_dir)
        self._initialized = True

    def _wait_rate_limit(self):
        elapsed = time.time() - self.last_request_time
        if elapsed < self.min_delay_sec:
            sleep_time = self.min_delay_sec - elapsed
            print(f"[WebLLM] Ожидание лимитов браузера ({sleep_time:.1f}s)...", file=sys.stderr)
            time.sleep(sleep_time)
        self.last_request_time = time.time()

    def query(self, prompt: str, image_path: str = None) -> str:
        self._wait_rate_limit()

        if "deepseek" in self.provider:
            return self._query_semantic(prompt, "https://chat.deepseek.com/", image_path)
        elif "chatgpt" in self.provider:
            return self._query_semantic(prompt, "https://chatgpt.com/", image_path)
        elif "gemini" in self.provider:
            return self._query_semantic(prompt, "https://gemini.google.com/app", image_path)
        else:
            raise ValueError(f"Unknown web provider: {self.provider}")

    def _query_semantic(self, prompt: str, url: str, image_path: str) -> str:
        """Универсальный семантический запрос к WebLLM через Affordances."""

        # 1. Навигация в среде
        res = self.env.act("navigate", {"url": url})
        obs = res.get("observation", {})

        # 2. Опционально загружаем картинку
        if image_path and os.path.exists(image_path):
            self.env.act("upload_file", {"path": image_path})

        # 3. Поиск поля ввода среди аффордансов
        input_affordance = None
        for a_id, aff in obs.get("affordances", {}).items():
            if aff["type"] == "type_text":
                input_affordance = a_id
                # Нашли первое попавшееся поле (обычно чаты имеют одно большое поле)
                break

        if not input_affordance:
            print("[WebLLM] Ошибка: Не найдено поле ввода на странице!", file=sys.stderr)
            return ""

        # 4. Ввод текста и нажатие Enter
        self.env.act(input_affordance, {"text": prompt, "key": "Enter"})

        # 5. Ожидание стабилизации ответа
        return self._wait_for_response(url)

    def _wait_for_response(self, url: str, timeout: int = 60) -> str:
        """Ждет, пока текстовый контент среды не перестанет меняться."""
        start_time = time.time()
        last_text = ""
        stable_count = 0

        while time.time() - start_time < timeout:
            res = self.env.act("wait", {"ms": 1000})
            obs = res.get("observation", {})

            # Читаем ВЕСЬ текст страницы. JSON-парсер (parse_json из sdk.py)
            # сам вырежет нужный кусок кода из этого месива!
            current_text = obs.get("state", {}).get("content", "")

            if current_text and current_text == last_text:
                stable_count += 1
                if stable_count >= 4:
                    return current_text
            else:
                stable_count = 0
                last_text = current_text

        return last_text

    def close(self):
        if self._initialized:
            self.env.close()
            self._initialized = False
