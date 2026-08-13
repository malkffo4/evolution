from __future__ import annotations

import hashlib
import time
from typing import Any, Dict

from environments.base import EnvironmentRuntime
from environments.browser.fabric import BrowserSession


class StealthChromiumEnvironment(EnvironmentRuntime):
    """Browser environment with strong session isolation and deterministic behavior.

    This implementation deliberately does not promise or implement WAF/anti-fraud
    evasion. It is intended for user-owned accounts, privacy isolation and
    authorized browser security testing.
    """

    def __init__(self, session: BrowserSession, tab_id: str = "main") -> None:
        self.session = session
        self.tab_id = tab_id
        if tab_id not in session.tabs:
            session.new_tab(tab_id)
        self.tab = session.get_tab(tab_id)
        self._affordance_map: Dict[str, Any] = {}

    def select_tab(self, tab_id: str) -> None:
        self.tab = self.session.get_tab(tab_id)
        self.tab_id = tab_id
        self._affordance_map.clear()

    def _generate_element_id(self, tag: str, index: int) -> str:
        raw = f"{self.tab.url}\x00{tag}\x00{index}"
        return "el_" + hashlib.sha256(raw.encode()).hexdigest()[:12]

    def observe(self) -> Dict[str, Any]:
        self.tab.wait.load_start()
        self.tab.wait.doc_loaded()
        self._affordance_map.clear()
        affordances: Dict[str, Any] = {}

        elements = self.tab.eles('tag:a, tag:button, tag:input, tag:textarea, @role=button')
        for idx, el in enumerate(elements):
            if not el.is_displayed:
                continue
            tag = el.tag
            el_id = self._generate_element_id(tag, idx)
            self._affordance_map[el_id] = el
            text = el.text or el.attr('placeholder') or el.attr('aria-label') or el.attr('value') or ""
            if tag in ("input", "textarea") and el.attr('type') not in ("button", "submit", "hidden"):
                kind = "type_text"
            elif tag == "a":
                kind = "navigate"
                text = text or el.attr('href') or ""
            else:
                kind = "activate"
            affordances[el_id] = {"type": kind, "label": text[:160].strip(), "tag": tag}

        return {
            "state": {
                "url": self.tab.url,
                "title": self.tab.title,
                "content": self.tab.html[:5000],
                "profile_id": self.session.profile.profile_id,
                "tab_id": self.tab_id,
            },
            "affordances": affordances,
        }

    def act(self, affordance_id: str, params: Dict[str, Any] | None = None) -> Dict[str, Any]:
        params = params or {}
        if affordance_id == "navigate":
            url = params.get("url")
            if not url:
                return {"success": False, "error": "URL missing"}
            self.tab.get(url)
            return {"success": True, "observation": self.observe()}

        element = self._affordance_map.get(affordance_id)
        if element is None:
            return {"success": False, "error": "Affordance expired; observe again"}

        try:
            element.scroll.to_see(center=True)
            time.sleep(0.15)
            if "text" in params:
                element.clear()
                element.input(str(params["text"]))
                if params.get("key") == "Enter":
                    from DrissionPage.common.keys import Keys
                    self.tab.actions.type(Keys.ENTER)
            else:
                element.click()
            return {"success": True, "observation": self.observe()}
        except Exception as exc:
            return {"success": False, "error": str(exc)}

    def close(self) -> None:
        self.session.close_tab(self.tab_id)
