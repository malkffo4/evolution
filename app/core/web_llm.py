# app/core/web_llm.py
import os
import time
from playwright.sync_api import sync_playwright

class WebLLMClient:
    """
    Управляет реальным браузером для доступа к ChatGPT, DeepSeek и Gemini.
    Сохраняет профиль в .browser_session, чтобы не разлогиниваться.
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
        
        print("[WebLLM] Запускаю Playwright... (если запускаешь впервые, браузер откроется для логина)")
        self.playwright = sync_playwright().start()
        self.browser = self.playwright.chromium.launch_persistent_context(
            user_data_dir=self.user_data_dir,
            headless=self.headless,
            args=["--disable-blink-features=AutomationControlled"],
            no_viewport=False
        )
        self.page = self.browser.pages[0] if self.browser.pages else self.browser.new_page()
        self._initialized = True

    def query(self, prompt: str) -> str:
        if "deepseek" in self.provider:
            return self._query_deepseek(prompt)
        elif "chatgpt" in self.provider:
            return self._query_chatgpt(prompt)
        elif "gemini" in self.provider:
            return self._query_gemini(prompt)
        else:
            raise ValueError(f"Unknown web provider: {self.provider}")

    def _wait_for_text_stabilization(self, selector: str, timeout: int = 60) -> str:
        """Ждет, пока текст в последнем элементе selector не перестанет меняться (завершение генерации)"""
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
                if stable_count >= 4:  # Текст не меняется 2 секунды (4 * 0.5s)
                    return current_text
            else:
                stable_count = 0
                last_text = current_text
                
            time.sleep(0.5)
            
        return last_text

    def _query_deepseek(self, prompt: str) -> str:
        self.page.goto("https://chat.deepseek.com/")
        self.page.wait_for_selector("textarea", timeout=15000)
        
        # Вводим текст
        self.page.fill("textarea", prompt)
        time.sleep(0.5)
        self.page.keyboard.press("Enter")
        
        # Ждем генерации. DeepSeek использует класс ds-markdown для ответов
        return self._wait_for_text_stabilization(".ds-markdown")

    def _query_chatgpt(self, prompt: str) -> str:
        self.page.goto("https://chatgpt.com/")
        self.page.wait_for_selector("#prompt-textarea", timeout=15000)
        
        self.page.fill("#prompt-textarea", prompt)
        time.sleep(0.5)
        self.page.keyboard.press("Enter")
        
        # Ответы в ChatGPT лежат в классе .markdown
        return self._wait_for_text_stabilization(".markdown")

    def _query_gemini(self, prompt: str) -> str:
        self.page.goto("https://gemini.google.com/app")
        # У Gemini специфичный contenteditable div
        self.page.wait_for_selector("rich-textarea", timeout=15000)
        
        self.page.click("rich-textarea")
        self.page.keyboard.type(prompt)
        time.sleep(0.5)
        self.page.keyboard.press("Enter")
        
        # Ответы в message-content
        return self._wait_for_text_stabilization("message-content")

    def close(self):
        if self._initialized:
            self.browser.close()
            self.playwright.stop()
            self._initialized = False