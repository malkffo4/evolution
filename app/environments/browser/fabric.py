from __future__ import annotations

import shutil
import sys
from dataclasses import dataclass, field
from pathlib import Path
from threading import RLock
from typing import Dict, Optional

from DrissionPage import Chromium, ChromiumOptions

from .profile import BrowserProfile
from environments.stealth_evasion import get_stealth_scripts


@dataclass
class BrowserSession:
    profile: BrowserProfile
    browser: Chromium
    tabs: Dict[str, object] = field(default_factory=dict)

    def new_tab(self, tab_id: str, url: Optional[str] = None):
        if not tab_id:
            raise ValueError("tab_id is required")
        if tab_id in self.tabs:
            raise ValueError(f"tab already exists: {tab_id}")

        tab = self.browser.new_tab(url=url)

        try:
            hw_profile = {
                "user_agent": self.profile.user_agent,
                "platform": "Win32",
                "cpu_cores": 8,
                "ram_gb": 16,
                "webgl_vendor": "Google Inc. (NVIDIA)",
                "webgl_renderer": "ANGLE (NVIDIA, NVIDIA GeForce RTX 3060 Direct3D11 vs_5_0, D3D11)",
                "canvas_noise": 1.000845,
            }
            stealth_js = get_stealth_scripts(hw_profile)
            tab.run_cdp("Page.addScriptToEvaluateOnNewDocument", source=stealth_js)
        except Exception as exc:
            print(f"[Fabric] Warning: Failed to inject stealth scripts: {exc}", file=sys.stderr)

        self.tabs[tab_id] = tab
        return tab

    def get_tab(self, tab_id: str):
        try:
            return self.tabs[tab_id]
        except KeyError as exc:
            raise KeyError(f"unknown tab {tab_id!r}") from exc

    def close_tab(self, tab_id: str) -> None:
        tab = self.tabs.pop(tab_id, None)
        if tab is not None:
            try:
                tab.close()
            except Exception:
                pass

    def close(self) -> None:
        # Closing the browser process gracefully is important here: Chromium
        # flushes cookies/localStorage/profile state during shutdown.
        for tab_id in list(self.tabs):
            self.close_tab(tab_id)
        try:
            self.browser.quit()
        except Exception as exc:
            print(f"[Fabric] Browser quit warning: {exc}", file=sys.stderr)


class BrowserFabric:
    """Orchestrates isolated Chromium processes backed by persistent profiles."""

    def __init__(self, root: str | Path = ".browser_profiles") -> None:
        self.root = Path(root).expanduser().resolve()
        self.root.mkdir(parents=True, exist_ok=True)
        self._sessions: Dict[str, BrowserSession] = {}
        self._lock = RLock()

    @staticmethod
    def _find_browser_path() -> Optional[str]:
        candidates = [
            "chromium",
            "chromium-browser",
            "google-chrome",
            "google-chrome-stable",
            "brave-browser",
            "/usr/bin/chromium",
            "/usr/bin/google-chrome",
        ]
        for name in candidates:
            path = shutil.which(name)
            if path:
                return path
        return None

    def open(self, profile: BrowserProfile, port: Optional[int] = None) -> BrowserSession:
        with self._lock:
            if profile.profile_id in self._sessions:
                raise RuntimeError(f"profile already active: {profile.profile_id}")

            profile.prepare()
            debug_port = int(port or profile.debug_port)

            # IMPORTANT: a stable port lets a new Python process RECONNECT to
            # the same Chromium process instead of starting a second browser.
            # DrissionPage documents that Chromium(port) takes over an existing
            # browser on that port.
            try:
                browser = Chromium(debug_port)
                print(
                    f"[Fabric] Reconnected to existing Chromium profile="
                    f"{profile.profile_id} port={debug_port}",
                    file=sys.stderr,
                )
                session = BrowserSession(profile=profile, browser=browser)
                self._sessions[profile.profile_id] = session
                return session
            except Exception:
                pass

            opts = ChromiumOptions()
            browser_path = self._find_browser_path()
            if browser_path:
                opts.set_browser_path(browser_path)

            opts.set_local_port(debug_port)
            opts.set_user_data_path(str(profile.user_data_dir))
            opts.set_argument("--no-sandbox")
            opts.set_argument("--disable-dev-shm-usage")
            opts.set_argument("--disable-popup-blocking")
            opts.set_argument("--no-first-run")
            opts.set_argument("--no-default-browser-check")
            opts.set_argument("--disable-session-crashed-bubble")
            opts.set_argument("--password-store=basic")
            opts.set_argument("--remote-allow-origins=*")

            if profile.user_agent:
                opts.set_user_agent(profile.user_agent)
            if profile.headless:
                opts.headless()
            if profile.proxy:
                opts.set_proxy(profile.proxy)
            if profile.download_dir:
                opts.set_download_path(str(profile.download_dir))

            browser = Chromium(addr_or_opts=opts)
            session = BrowserSession(profile=profile, browser=browser)
            self._sessions[profile.profile_id] = session
            return session

    def get(self, profile_id: str) -> BrowserSession:
        with self._lock:
            try:
                return self._sessions[profile_id]
            except KeyError as exc:
                raise KeyError(f"unknown profile: {profile_id!r}") from exc

    def detach(self, profile_id: str) -> None:
        """Drop Python references without terminating Chromium.

        This is the required mode for DeepSeek web login persistence because
        DeepSeek documents ds_session_id as a session cookie. Keeping Chromium
        alive keeps that browsing session alive across Python restarts.
        """
        with self._lock:
            self._sessions.pop(profile_id, None)

    def close(self, profile_id: str) -> None:
        with self._lock:
            session = self._sessions.pop(profile_id, None)
        if session:
            session.close()

    def close_all(self) -> None:
        with self._lock:
            sessions = list(self._sessions.items())
            self._sessions.clear()
        for _, session in sessions:
            session.close()
