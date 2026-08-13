from __future__ import annotations

import sys
from pathlib import Path
from typing import Dict, List

from environments.browser.fabric import BrowserFabric
from environments.browser.profile import BrowserProfile
from environments.browser.scope import TargetScope, validate_url_in_scope
from environments.stealth_web import StealthChromiumEnvironment
from environments.web import HttpReconEnvironment


class CapabilityDispatcher:
    """Routes browser/HTTP capabilities while enforcing target scope and isolation."""

    def __init__(
        self,
        scope: List[str] | None = None,
        profile_root: str | Path = ".browser_profiles",
    ) -> None:
        exact = set()
        roots = set()
        for item in scope or []:
            value = item.strip().lower().rstrip(".")
            if value.startswith("*."):
                roots.add(value[2:])
            else:
                exact.add(value)
        self.security_policy = TargetScope(frozenset(exact), frozenset(roots))
        self.fabric = BrowserFabric(profile_root)
        self._http: Dict[str, HttpReconEnvironment] = {}
        self._browser: Dict[tuple[str, str], StealthChromiumEnvironment] = {}

    def _browser_runtime(self, session_id: str, profile: dict | None, tab_id: str) -> StealthChromiumEnvironment:
        key = (session_id, tab_id)
        if key in self._browser:
            runtime = self._browser[key]
            runtime.select_tab(tab_id)
            return runtime

        cfg = profile or {}
        profile_id = str(cfg.get("profile_id") or session_id)
        data_dir = Path(cfg.get("user_data_dir") or (self.fabric.root / profile_id))
        bp = BrowserProfile(
            profile_id=profile_id,
            user_data_dir=data_dir,
            user_agent=cfg.get("user_agent"),
            locale=cfg.get("locale", "en-US"),
            timezone=cfg.get("timezone", "UTC"),
            headless=bool(cfg.get("headless", False)),
            proxy=cfg.get("proxy"),
            download_dir=cfg.get("download_dir"),
        )
        try:
            session = self.fabric.get(profile_id)
        except KeyError:
            session = self.fabric.open(bp)
        if tab_id not in session.tabs:
            session.new_tab(tab_id)
        runtime = StealthChromiumEnvironment(session, tab_id)
        self._browser[key] = runtime
        return runtime

    def invoke(self, payload: dict) -> dict:
        capability = payload.get("capability")
        session_id = payload.get("session_id", "default")
        action = payload.get("action")
        params = payload.get("params") or {}
        args = params.get("args") or {}

        if not capability or not action:
            return {"ok": False, "error": "Missing 'capability' or 'action'"}

        if action == "act" and params.get("affordance_id") == "navigate":
            url = args.get("url") or params.get("url")
            try:
                validate_url_in_scope(url, self.security_policy)
            except PermissionError as exc:
                print(f"[SECURITY BLOCK] {exc}", file=sys.stderr)
                return {"ok": False, "error": str(exc)}

        try:
            if capability == "FetchRawHtml":
                env = self._http.setdefault(session_id, HttpReconEnvironment())
            elif capability == "ObserveVisualWebState":
                tab_id = str(params.get("tab_id") or "main")
                env = self._browser_runtime(session_id, params.get("profile_config"), tab_id)
            else:
                return {"ok": False, "error": f"Unknown capability: {capability}"}

            if action == "observe":
                return {"ok": True, "observation": env.observe()}
            if action == "act":
                result = env.act(params.get("affordance_id"), args)
                return {"ok": bool(result.get("success")), "result": result}
            if action == "close":
                if capability == "FetchRawHtml":
                    env.close()
                    self._http.pop(session_id, None)
                else:
                    tab_id = str(params.get("tab_id") or "main")
                    key = (session_id, tab_id)
                    runtime = self._browser.pop(key, None)
                    if runtime:
                        runtime.session.close_tab(tab_id)
                    if not any(k[0] == session_id for k in self._browser):
                        try:
                            self.fabric.close(str((params.get("profile_config") or {}).get("profile_id") or session_id))
                        except Exception:
                            pass
                return {"ok": True}
            return {"ok": False, "error": f"Unknown action: {action}"}
        except Exception as exc:
            return {"ok": False, "error": str(exc)}

    def shutdown_all(self) -> None:
        for env in list(self._http.values()):
            try:
                env.close()
            except Exception:
                pass
        self._http.clear()
        self._browser.clear()
        self.fabric.close_all()
