# app/core/web_llm.py
import os
import sys
import time
from playwright.sync_api import sync_playwright  # ФИКС: был async_api

class WebLLMClient:
    """
    Управляет реальным браузером для доступа к ChatGPT, DeepSeek и Gemini.
    Поддерживает Rate Limiting и загрузку изображений (Vision).
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
        self.user_data_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".browser_session"))

        # Ограничитель: минимум 12 секунд между запросами для web-интерфейсов
        self.last_request_time = 0
        self.min_delay_sec = 12.0

        print(f"[WebLLM] Запускаю Playwright для {provider}... (первый запуск может потребовать логина)")
        self.playwright = sync_playwright().start()
        self.browser = self.playwright.chromium.launch_persistent_context(
            user_data_dir=self.user_data_dir,
            headless=self.headless,
            args=["--disable-blink-features=AutomationControlled"],
            no_viewport=False
        )
        self.page = self.browser.pages[0] if self.browser.pages else self.browser.new_page()
        self._initialized = True

    def _wait_rate_limit(self):
        """Защита от спама и блокировок в web-интерфейсах."""
        elapsed = time.time() - self.last_request_time
        if elapsed < self.min_delay_sec:
            sleep_time = self.min_delay_sec - elapsed
            print(f"[WebLLM] Ожидание лимитов браузера ({sleep_time:.1f}s)...", file=sys.stderr)
            time.sleep(sleep_time)
        self.last_request_time = time.time()

    def _upload_image_if_present(self, image_path: str):
        """Пытается найти скрытый input для файлов и загрузить картинку."""
        if not image_path or not os.path.exists(image_path):
            return
        try:
            self.page.set_input_files('input[type="file"]', image_path, timeout=5000)
            print(f"[WebLLM] Изображение {os.path.basename(image_path)} прикреплено.")
            time.sleep(3) # Ждем пока UI отрендерит превью картинки
        except Exception as e:
            print(f"[WebLLM] Не удалось загрузить картинку в UI: {e}", file=sys.stderr)

    def query(self, prompt: str, image_path: str = None) -> str:
        self._wait_rate_limit()

        if "deepseek" in self.provider:
            return self._query_deepseek(prompt, image_path)
        elif "chatgpt" in self.provider:
            return self._query_chatgpt(prompt, image_path)
        elif "gemini" in self.provider:
            return self._query_gemini(prompt, image_path)
        else:
            raise ValueError(f"Unknown web provider: {self.provider}")

    def _wait_for_text_stabilization(self, selector: str, timeout: int = 60) -> str:
        """Ждет, пока текст в последнем элементе selector не перестанет меняться."""
        start_time = time.time()
        last_text = ""
        stable_count = 0

        self.page.wait_for_selector(selector, state="attached", timeout=timeout*1000)

        while time.time() - start_time < timeout:
            elements = self.page.query_selector_all(selector)
            if not elements:
                time.sleep(1)
                continue

            current_text = elements[-1].inner_text()
            if current_text and current_text == last_text:
                stable_count += 1
                if stable_count >= 4:
                    return current_text
            else:
                stable_count = 0
                last_text = current_text

            time.sleep(0.5)

        return last_text

    def _query_deepseek(self, prompt: str, image_path: str) -> str:
        self.page.goto("https://chat.deepseek.com/")
        self.page.wait_for_selector("textarea", timeout=15000)
        self._upload_image_if_present(image_path)
        self.page.fill("textarea", prompt)
        time.sleep(0.5)
        self.page.keyboard.press("Enter")
        return self._wait_for_text_stabilization(".ds-markdown")

    def _query_chatgpt(self, prompt: str, image_path: str) -> str:
        self.page.goto("https://chatgpt.com/")
        self.page.wait_for_selector("#prompt-textarea", timeout=15000)
        self._upload_image_if_present(image_path)
        self.page.fill("#prompt-textarea", prompt)
        time.sleep(0.5)
        self.page.keyboard.press("Enter")
        return self._wait_for_text_stabilization(".markdown")

    def _query_gemini(self, prompt: str, image_path: str) -> str:
        self.page.goto("https://gemini.google.com/app")
        self.page.wait_for_selector("rich-textarea", timeout=15000)
        self._upload_image_if_present(image_path)
        self.page.click("rich-textarea")
        self.page.keyboard.type(prompt)
        time.sleep(0.5)
        self.page.keyboard.press("Enter")
        return self._wait_for_text_stabilization("message-content")

    def close(self):
        if self._initialized:
            self.browser.close()
            self.playwright.stop()
            self._initialized = False
