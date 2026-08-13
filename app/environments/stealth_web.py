# app/environments/stealth_web.py
import time
import hashlib
import undetected_chromedriver as uc
from selenium.webdriver.common.by import By
from selenium.webdriver.common.keys import Keys
from bs4 import BeautifulSoup
from typing import Dict, Any

from .base import EnvironmentRuntime

class StealthWebEnvironment(EnvironmentRuntime):
    """
    Тяжелая реализация среды с реальным V8 и обходом антибот-защит.
    Использует undetected-chromedriver.
    """
    def __init__(self, headless: bool = False, profile_dir: str = None):
        options = uc.ChromeOptions()
        if headless:
            options.add_argument('--headless')

        # Базовая оптимизация и маскировка
        options.add_argument('--disable-popup-blocking')
        options.add_argument('--no-first-run')
        options.add_argument('--no-service-autorun')
        options.add_argument('--password-store=basic')

        if profile_dir:
            options.add_argument(f"--user-data-dir={profile_dir}")

        self.driver = uc.Chrome(options=options)
        self.driver.implicitly_wait(3)

        # Маппинг: Affordance ID -> XPath для Selenium
        self._affordance_map: Dict[str, dict] = {}

    def _generate_element_id(self, tag_name: str, index: int) -> str:
        raw = f"{self.driver.current_url}_{tag_name}_{index}"
        return f"el_{hashlib.md5(raw.encode()).hexdigest()[:8]}"

    def _get_xpath(self, element) -> str:
        """Простой и быстрый генератор XPath из элемента BeautifulSoup."""
        components = []
        child = element if element.name else element.parent
        for parent in child.parents:
            if parent.name == '[document]': break
            siblings = parent.find_all(child.name, recursive=False)
            if len(siblings) == 1:
                components.append(child.name)
            else:
                components.append(f"{child.name}[{1 + siblings.index(child)}]")
            child = parent
        components.reverse()
        return f"/{'/'.join(components)}"

    def observe(self) -> Dict[str, Any]:
        """Возвращает семантическое состояние, ожидая прохождения Cloudflare."""
        # Даем время на выполнение JS-челленджей
        time.sleep(2.5)

        html = self.driver.page_source
        soup = BeautifulSoup(html, "html.parser")

        self._affordance_map.clear()
        affordances = {}

        # 1. Извлекаем ссылки (Навигация)
        for idx, a_tag in enumerate(soup.find_all("a", href=True)):
            if not a_tag.is_visible if hasattr(a_tag, 'is_visible') else False: # Упрощенная проверка
                pass
            el_id = self._generate_element_id("a", idx)
            text = a_tag.get_text(strip=True) or a_tag["href"]

            self._affordance_map[el_id] = {"action": "click", "xpath": self._get_xpath(a_tag)}
            affordances[el_id] = {"type": "navigate", "label": text[:100], "tag": "a"}

        # 2. Извлекаем инпуты и кнопки
        for idx, inp in enumerate(soup.find_all(["input", "textarea", "button"])):
            inp_type = inp.get("type", "text").lower() if inp.name == "input" else None
            if inp_type == "hidden":
                continue

            el_id = self._generate_element_id(inp.name, idx)
            xpath = self._get_xpath(inp)
            label = inp.get("placeholder") or inp.get("aria-label") or inp.get("name") or inp.get_text(strip=True) or "element"

            if inp.name == "button" or inp_type in ["submit", "button"]:
                self._affordance_map[el_id] = {"action": "click", "xpath": xpath}
                affordances[el_id] = {"type": "activate", "label": label[:50], "tag": inp.name}
            else:
                self._affordance_map[el_id] = {"action": "fill", "xpath": xpath}
                affordances[el_id] = {"type": "type_text", "label": label[:50], "tag": inp.name}

        text_content = soup.get_text(separator=' ', strip=True)

        return {
            "state": {
                "url": self.driver.current_url,
                "title": self.driver.title,
                "content": text_content[:2000] # Даем ядру контекст для понимания
            },
            "affordances": affordances
        }

    def act(self, affordance_id: str, params: Dict[str, Any] = None) -> Dict[str, Any]:
        params = params or {}

        # Глобальный аффорданс навигации
        if affordance_id == "navigate":
            url = params.get("url")
            if not url: return {"success": False, "error": "URL missing"}
            try:
                self.driver.get(url)
                return {"success": True, "observation": self.observe()}
            except Exception as e:
                return {"success": False, "error": str(e)}

        if affordance_id not in self._affordance_map:
            return {"success": False, "error": f"Affordance {affordance_id} not found"}

        target = self._affordance_map[affordance_id]

        try:
            element = self.driver.find_element(By.XPATH, target["xpath"])
            # Скроллим к элементу, обходя перекрывающие хэдеры
            self.driver.execute_script("arguments[0].scrollIntoView({block: 'center'});", element)
            time.sleep(0.5)

            if target["action"] == "click":
                try:
                    element.click()
                except:
                    # Fallback на JS-клик, если элемент перекрыт (часто бывает в SPA)
                    self.driver.execute_script("arguments[0].click();", element)

            elif target["action"] == "fill":
                element.clear()
                element.send_keys(params.get("text", ""))
                if params.get("key") == "Enter":
                    element.send_keys(Keys.ENTER)

            time.sleep(1.5) # Даем SPA-фреймворкам отрендерить новые элементы
            return {"success": True, "observation": self.observe()}

        except Exception as e:
            return {"success": False, "error": str(e)}

    def close(self):
        try:
            self.driver.quit()
        except:
            pass
