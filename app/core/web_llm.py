from __future__ import annotations

import atexit
import os
import sys
import time
from pathlib import Path

from DrissionPage.common import Keys

from environments.browser.fabric import BrowserFabric
from environments.browser.profile import BrowserProfile


class WebLLMClient:
    """Drive an authenticated web LLM through a persistent Chromium profile.

    Important behavior:
      * the DeepSeek image path is actually attached to the web chat;
      * response detection is based on the real DeepSeek message DOM, not a JS
        function object accidentally handed to run_js();
      * Chromium is detached, not killed, on Python shutdown so DeepSeek's
        session cookie remains alive between Python runs.
    """

    _instances: dict[str, "WebLLMClient"] = {}

    def __new__(cls, *args, **kwargs):
        provider = kwargs.get("provider") or (args[0] if args else "web_deepseek")
        if provider not in cls._instances:
            obj = super().__new__(cls)
            obj._initialized = False
            cls._instances[provider] = obj
        return cls._instances[provider]

    def __init__(self, provider: str = "web_deepseek", headless: bool = False):
        if self._initialized:
            if self.provider != provider:
                raise RuntimeError(
                    f"WebLLMClient already initialized for {self.provider}, requested {provider}"
                )
            return

        self.provider = provider
        self.headless = headless
        base_dir = Path(__file__).resolve().parents[2] / ".browser_session_llm"
        base_dir.mkdir(parents=True, exist_ok=True)
        self.fabric = BrowserFabric(base_dir)
        self.user_data_dir = (base_dir / f"profile_{provider}").resolve()
        self.profile = BrowserProfile(
            profile_id=f"llm_{provider}",
            user_data_dir=self.user_data_dir,
            headless=self.headless,
            user_agent=(
                "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                "AppleWebKit/537.36 (KHTML, like Gecko) "
                "Chrome/120.0.0.0 Safari/537.36"
            ),
        )

        self.last_request_time = 0.0
        self.min_delay_sec = 5.0
        self.urls = {
            "web_deepseek": "https://chat.deepseek.com/",
            "web_chatgpt": "https://chatgpt.com/",
            "web_gemini": "https://gemini.google.com/app",
        }
        self.target_url = self.urls.get(provider, "https://chat.deepseek.com/")

        print(f"[WebLLM] Запускаю браузер для {provider}...", file=sys.stderr)
        try:
            self.session = self.fabric.get(self.profile.profile_id)
        except KeyError:
            self.session = self.fabric.open(self.profile)

        try:
            self.tab = self.session.get_tab("chat_main")
        except KeyError:
            self.tab = self.session.new_tab("chat_main")

        if not self.tab.url or self.tab.url == "about:blank":
            try:
                self.tab.get(self.target_url)
            except Exception:
                pass

        atexit.register(self.close)
        self._initialized = True

    def _wait_rate_limit(self) -> None:
        elapsed = time.time() - self.last_request_time
        if elapsed < self.min_delay_sec:
            time.sleep(self.min_delay_sec - elapsed)
        self.last_request_time = time.time()

    def _refresh_tab_reference(self) -> None:
        try:
            latest = self.session.browser.latest_tab
            if latest:
                self.tab = self.session.browser.get_tab(latest)
                return
        except Exception:
            pass
        try:
            self.tab = self.session.get_tab("chat_main")
        except Exception:
            if "chat_main" not in self.session.tabs:
                self.tab = self.session.new_tab("chat_main")
            else:
                self.tab = self.session.tabs["chat_main"]

    def _get_chat_input(self):
        self._refresh_tab_reference()
        if "chatgpt" in self.provider:
            selectors = (
                '#prompt-textarea',
                'xpath://*[@id="prompt-textarea"]',
                'xpath://div[@contenteditable="true"]',
                'tag:textarea'
            )
        elif "deepseek" in self.provider:
            selectors = (
                '#chat-input',
                'xpath://textarea[@id="chat-input"]',
                'xpath://textarea',
                'xpath://div[@contenteditable="true"]',
                'tag:textarea'
            )
        elif "gemini" in self.provider:
            selectors = (
                '.ql-editor',
                'xpath://div[@contenteditable="true"]',
                'tag:textarea',
            )
        else:
            selectors = ('tag:textarea', 'xpath://div[@contenteditable="true"]')

        for selector in selectors:
            try:
                el = self.tab.ele(selector)
                if not el:
                    continue

                # Безопасная проверка видимости (поддержка DP 4.x и 3.x)
                is_visible = True
                try:
                    if hasattr(el, 'states'):
                        is_visible = el.states.is_displayed
                    elif hasattr(el, 'is_displayed'):
                        is_visible = el.is_displayed() if callable(el.is_displayed) else el.is_displayed
                except Exception:
                    pass

                if is_visible:
                    return el
            except Exception:
                continue
        return None

    def _assistant_messages(self) -> list[str]:
        """Extract only assistant messages from the current DOM."""
        self._refresh_tab_reference()
        messages: list[str] = []

        # 1. Специфично для DeepSeek
        try:
            # Обязательно префикс css: для DrissionPage 4.x
            for msg in self.tab.eles('css:.ds-message'):
                try:
                    md = msg.ele('css:.ds-assistant-message-main-content', timeout=0)
                except Exception:
                    md = None
                if md:
                    text = (md.text or '').strip()
                    if text:
                        messages.append(text)
            if messages:
                return messages
        except Exception:
            pass

        # 2. Специфично для ChatGPT и универсальный fallback
        try:
            # Обязательно префикс css:
            for node in self.tab.eles('css:[data-message-author-role="assistant"]'):
                # В ChatGPT текст часто лежит внутри вложенного .markdown
                try:
                    md = node.ele('css:.markdown', timeout=0)
                    text = (md.text or '').strip() if md else (node.text or '').strip()
                except Exception:
                    text = (node.text or '').strip()

                if text:
                    messages.append(text)
            if messages:
                return messages
        except Exception:
            pass

        # 3. Fallback для старых версий верстки DeepSeek
        try:
            for node in self.tab.eles('css:.ds-message .ds-markdown'):
                text = (node.text or '').strip()
                if text:
                    messages.append(text)
        except Exception:
            pass

        return messages

    def _assistant_snapshot(self) -> tuple[int, list[str]]:
        texts = self._assistant_messages()
        return len(texts), texts

    def _compose_prompt(self, prompt: str, system: str | None, json_mode: bool) -> str:
        # Keep exactly one copy of the instructions. llm.py intentionally does
        # not add any wrapper around this text.
        parts = []
        if system:
            parts.append(system.strip())
        parts.append(prompt.strip())
        if json_mode:
            parts.append("Ответь СТРОГО в формате валидного JSON без markdown-обрамления.")
        return "\n\n".join(p for p in parts if p)

    def _attach_file(self, image_path: str) -> None:
        """Attach a local image/file through the site's own file input."""
        if not image_path or not os.path.exists(image_path):
            raise FileNotFoundError(f"Attachment not found: {image_path}")

        # Исправленные селекторы: в DrissionPage 4.x обязательны префиксы!
        selectors = (
            'tag:input@type=file',
            'css:input[type="file"]',
            'xpath://input[@type="file"]',
            'css:input[accept*="image"]'
        )

        file_input = None
        for selector in selectors:
            try:
                # В DrissionPage ele() ищет скрытые элементы (file input всегда скрыт)
                file_input = self.tab.ele(selector)
                if file_input:
                    break
            except Exception:
                continue

        if not file_input:
            raise RuntimeError(
                f"Could not find a file upload input in {self.provider} chat DOM. "
                "Possibly unsupported by provider or UI changed."
            )

        file_input.input(str(Path(image_path).resolve()))

        # Ждем, пока React-фронтенд отрендерит превью загруженного файла
        deadline = time.monotonic() + 20.0
        filename = Path(image_path).name
        while time.monotonic() < deadline:
            try:
                html = self.tab.html
                lower = html.lower()
                if filename.lower() in lower:
                    return

                # Здесь также исправлены селекторы (добавлен префикс css:)
                for selector in (
                    'css:img[src^="blob:"]',
                    'css:[class*="attachment"]',
                    'css:[class*="upload"]',
                    'css:[data-file-name]',
                ):
                    if self.tab.ele(selector):
                        return
            except Exception:
                pass
            time.sleep(0.5)

        raise TimeoutError(f"Timed out waiting for upload of {filename}")

    def query(
        self,
        prompt: str,
        system: str = None,
        json_mode: bool = True,
        timeout: int = 120,
        image_path: str = None,
    ) -> str:
        self._wait_rate_limit()
        composed = self._compose_prompt(prompt, system, json_mode)
        return self._query_tab(composed, image_path=image_path, timeout=timeout)

    def _query_tab(self, prompt: str, image_path: str = None, timeout: int = 120) -> str:
        print(f"\n[WebLLM] Ожидаю готовности чата {self.provider}...", file=sys.stderr)

        deadline = time.monotonic() + min(timeout, 300)
        input_el = None
        while time.monotonic() < deadline:
            input_el = self._get_chat_input()
            if input_el:
                break
            print(
                "[WebLLM] Чат не готов. Если страница просит логин — "
                "авторизуйтесь в открытом окне. Жду...",
                file=sys.stderr,
            )
            time.sleep(2.0)

        if not input_el:
            raise TimeoutError(f"Timed out waiting for {self.provider} chat input")

        before_count, before_texts = self._assistant_snapshot()

        if image_path:
            self._attach_file(image_path)

        try:
            input_el.clear()
        except Exception:
            pass
        input_el.input(prompt)
        time.sleep(0.3)

        # User confirmed that Enter already submits correctly in the running
        # DeepSeek UI, so keep the browser's native submission path.
        try:
            input_el.input(Keys.ENTER)
        except Exception:
            self.tab.actions.type(Keys.ENTER)

        return self._wait_for_response(before_count, before_texts, timeout)

    def _wait_for_response(
        self,
        before_count: int,
        before_texts: list[str],
        timeout: int = 120,
    ) -> str:
        deadline = time.monotonic() + timeout
        last_text = ''
        stable_count = 0
        saw_new = False

        while time.monotonic() < deadline:
            time.sleep(0.8)
            try:
                count, texts = self._assistant_snapshot()
                current = texts[-1].strip() if texts else ''

                if count > before_count:
                    saw_new = True
                elif count == before_count and before_texts and current:
                    # Streaming can mutate the same assistant node in place.
                    if current != before_texts[-1].strip():
                        saw_new = True

                if not saw_new or not current:
                    continue

                if current == last_text:
                    stable_count += 1
                else:
                    last_text = current
                    stable_count = 0

                if stable_count >= 3:
                    return current
            except Exception:
                continue

        raise TimeoutError(
            f"Timed out waiting for a new {self.provider} assistant response "
            f"(assistant_nodes_before={before_count})"
        )

    def close(self) -> None:
        """Detach Python from Chromium; do NOT kill the browser process.

        DeepSeek documents ds_session_id as a session cookie. Keeping the same
        Chromium process alive is therefore what preserves this web login.
        """
        if not self._initialized:
            return
        try:
            self.fabric.detach(self.profile.profile_id)
        except Exception:
            pass
        self._initialized = False
