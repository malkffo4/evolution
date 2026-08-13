# app/environments/web.py
import time
import hashlib
import os
from typing import Dict, Any
from playwright.sync_api import sync_playwright, Page, ElementHandle

from .base import EnvironmentRuntime

class WebEnvironment(EnvironmentRuntime):
    """
    Реализация EnvironmentRuntime для Веба.
    Изолирует KOSMOS от DOM, Playwright и CSS-селекторов.
    """
    def __init__(self, headless: bool = False, user_data_dir: str = None):
        self.playwright = sync_playwright().start()

        # Для веб-LLM нам нужна персистентная сессия (чтобы сохранять логин)
        if user_data_dir:
            os.makedirs(user_data_dir, exist_ok=True)
            self.browser = self.playwright.chromium.launch_persistent_context(
                user_data_dir=user_data_dir,
                headless=headless,
                args=["--disable-blink-features=AutomationControlled"],
                no_viewport=False
            )
            self.page: Page = self.browser.pages[0] if self.browser.pages else self.browser.new_page()
        else:
            self.browser = self.playwright.chromium.launch(
                headless=headless,
                args=["--disable-blink-features=AutomationControlled"]
            )
            self.context = self.browser.new_context()
            self.page: Page = self.context.new_page()

        self._affordance_map: Dict[str, ElementHandle] = {}

    def _generate_element_id(self, element: ElementHandle) -> str:
        """Генерирует стабильный короткий ID для элемента (KOSMOS не видит DOM-узлы)."""
        try:
            tag = element.evaluate("el => el.tagName.toLowerCase()")
            box = element.bounding_box()
            coords = f"{box['x']}_{box['y']}" if box else str(time.time())
            raw = f"{tag}_{coords}"
            return f"el_{hashlib.md5(raw.encode()).hexdigest()[:8]}"
        except:
            return f"el_{hashlib.md5(str(time.time()).encode()).hexdigest()[:8]}"

    def observe(self) -> Dict[str, Any]:
        """Возвращает семантическое состояние страницы и доступные действия."""
        try:
            self.page.wait_for_load_state("domcontentloaded", timeout=3000)
        except Exception:
            pass # Игнорируем, если страница загрузилась не до конца

        self._affordance_map.clear()
        affordances = {}

        # Ищем интерактивные элементы. В будущем заменим на Accessibility Tree (AX Tree)
        selectors = "button, a, input, textarea, [role='button'], [role='textbox']"
        elements = self.page.query_selector_all(selectors)

        for el in elements:
            try:
                if not el.is_visible():
                    continue

                el_id = self._generate_element_id(el)
                self._affordance_map[el_id] = el

                tag_name = el.evaluate("el => el.tagName.toLowerCase()")
                input_type = el.evaluate("el => el.type") if tag_name == "input" else None

                # Читаем текст внутри или placeholder
                text = el.inner_text().strip() or el.get_attribute("placeholder") or el.get_attribute("aria-label") or ""

                if tag_name == "textarea" or (tag_name == "input" and input_type in ["text", "search", "password", "email"]):
                    aff_type = "type_text"
                else:
                    aff_type = "activate"

                affordances[el_id] = {
                    "type": aff_type,
                    "label": text[:100],
                    "tag": tag_name
                }
            except Exception:
                continue

        # Читаем весь текстовый контент страницы для экстракции знаний
        try:
            full_text = self.page.evaluate("document.body.innerText")
        except:
            full_text = ""

        return {
            "state": {
                "url": self.page.url,
                "title": self.page.title(),
                "content": full_text
            },
            "affordances": affordances
        }

    def act(self, affordance_id: str, params: Dict[str, Any] = None) -> Dict[str, Any]:
        """Выполняет действие над абстрактным ID."""
        if affordance_id == "navigate":
            url = params.get("url")
            if not url: return {"success": False, "error": "URL missing"}
            try:
                self.page.goto(url)
                return {"success": True, "observation": self.observe()}
            except Exception as e:
                return {"success": False, "error": str(e)}

        if affordance_id == "wait":
            self.page.wait_for_timeout(params.get("ms", 1000))
            return {"success": True, "observation": self.observe()}

        if affordance_id == "upload_file":
            try:
                self.page.set_input_files('input[type="file"]', params["path"], timeout=5000)
                self.page.wait_for_timeout(2000)
                return {"success": True, "observation": self.observe()}
            except Exception as e:
                return {"success": False, "error": f"Upload failed: {str(e)}"}

        if affordance_id not in self._affordance_map:
            return {"success": False, "error": f"Affordance {affordance_id} not found"}

        element = self._affordance_map[affordance_id]
        try:
            element.scroll_into_view_if_needed()

            if params and "text" in params:
                element.fill(params["text"])

            if params and "key" in params:
                element.press(params["key"])

            if not params or ("text" not in params and "key" not in params):
                element.click()

            self.page.wait_for_timeout(500) # Даем JS время на реакцию
            return {"success": True, "observation": self.observe()}
        except Exception as e:
            return {"success": False, "error": f"Action failed: {str(e)}"}

    def close(self):
        try:
            if hasattr(self, 'context') and self.context:
                self.context.close()
            self.browser.close()
            self.playwright.stop()
        except:
            pass
