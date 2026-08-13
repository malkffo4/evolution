# app/environments/web.py
import time
import hashlib
from typing import Dict, Any
from playwright.sync_api import sync_playwright, Page, ElementHandle

from .base import EnvironmentRuntime

class WebEnvironment(EnvironmentRuntime):
    """
    Реализация EnvironmentRuntime для Веба.
    Изолирует KOSMOS от DOM, Playwright и селекторов.
    """
    def __init__(self, headless: bool = False):
        self.playwright = sync_playwright().start()
        self.browser = self.playwright.chromium.launch(
            headless=headless,
            args=["--disable-blink-features=AutomationControlled"]
        )
        self.context = self.browser.new_context()
        self.page: Page = self.context.new_page()

        # Внутренний маппинг: Affordance ID -> Playwright ElementHandle
        # KOSMOS видит только ID, он не знает про ElementHandle
        self._affordance_map: Dict[str, ElementHandle] = {}

    def _generate_element_id(self, element: ElementHandle) -> str:
        """Генерирует стабильный короткий ID для элемента."""
        # Для простоты используем хэш от bounding box и tag_name
        box = element.bounding_box()
        tag = element.evaluate("el => el.tagName.toLowerCase()")
        raw = f"{tag}_{box['x']}_{box['y']}" if box else str(time.time())
        return f"el_{hashlib.md5(raw.encode()).hexdigest()[:6]}"

    def observe(self) -> Dict[str, Any]:
        """Сканирует страницу и возвращает семантическое состояние."""
        # Ждем загрузки сети
        self.page.wait_for_load_state("networkidle", timeout=5000)

        self._affordance_map.clear()
        affordances = {}

        # Ищем все интерактивные элементы (кнопки, ссылки, поля ввода)
        # В идеале здесь нужно использовать Accessibility Tree (AX Tree),
        # но для начала хватит простых селекторов.
        elements = self.page.query_selector_all("button, a, input, textarea, [role='button']")

        for el in elements:
            if not el.is_visible():
                continue

            el_id = self._generate_element_id(el)
            self._affordance_map[el_id] = el

            tag_name = el.evaluate("el => el.tagName.toLowerCase()")
            input_type = el.evaluate("el => el.type") if tag_name == "input" else None
            text = el.inner_text().strip() or el.get_attribute("placeholder") or el.get_attribute("aria-label") or ""

            # Определяем тип Affordance
            if tag_name in ["input", "textarea"] and input_type not in ["submit", "button", "checkbox", "radio"]:
                aff_type = "type_text"
            else:
                aff_type = "activate" # Клик/нажатие

            affordances[el_id] = {
                "type": aff_type,
                "label": text[:50], # Ограничиваем длину
                "tag": tag_name
            }

        return {
            "state": {
                "url": self.page.url,
                "title": self.page.title(),
                "content_snippet": self.page.evaluate("document.body.innerText")[:1000] # Даем ядру контекст
            },
            "affordances": affordances
        }

    def act(self, affordance_id: str, params: Dict[str, Any] = None) -> Dict[str, Any]:
        """Выполняет действие по абстрактному ID."""
        if affordance_id == "navigate":
            # Специальный глобальный аффорданс для среды
            url = params.get("url")
            if not url:
                return {"success": False, "error": "URL parameter missing"}
            try:
                self.page.goto(url)
                return {"success": True, "observation": self.observe()}
            except Exception as e:
                return {"success": False, "error": str(e)}

        if affordance_id not in self._affordance_map:
            return {"success": False, "error": f"Affordance {affordance_id} not found or no longer valid"}

        element = self._affordance_map[affordance_id]

        try:
            # Скроллим к элементу, чтобы он точно был в viewport
            element.scroll_into_view_if_needed()

            if params and "text" in params:
                element.fill(params["text"])
            else:
                element.click()

            # Даем время на рендеринг/переход после действия
            self.page.wait_for_timeout(1000)

            return {"success": True, "observation": self.observe()}

        except Exception as e:
            return {"success": False, "error": f"Action failed: {str(e)}"}

    def close(self):
        self.context.close()
        self.browser.close()
        self.playwright.stop()
