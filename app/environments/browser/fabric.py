from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from threading import RLock
from typing import Dict, Optional

from DrissionPage import Chromium, ChromiumOptions

from .profile import BrowserProfile


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
            tab.close()

    def close(self) -> None:
        for tab in list(self.tabs.values()):
            try:
                tab.close()
            except Exception:
                pass
        self.tabs.clear()
        try:
            self.browser.quit()
        except Exception:
            pass


class BrowserFabric:
    """Owns independent Chromium processes/profiles and their tabs.

    A profile is never shared by two browser processes.
    """

    def __init__(self, root: str | Path = ".browser_profiles") -> None:
        self.root = Path(root).expanduser().resolve()
        self.root.mkdir(parents=True, exist_ok=True)
        self._sessions: Dict[str, BrowserSession] = {}
        self._lock = RLock()

    def open(self, profile: BrowserProfile, port: Optional[int] = None) -> BrowserSession:
        with self._lock:
            if profile.profile_id in self._sessions:
                raise RuntimeError(f"profile already active: {profile.profile_id}")

            profile.prepare()
            opts = ChromiumOptions()
            opts.set_user_data_path(str(profile.user_data_dir))
            if profile.user_agent:
                opts.set_user_agent(profile.user_agent)
            if profile.headless:
                opts.headless()
            if profile.proxy:
                opts.set_proxy(profile.proxy)
            if profile.download_dir:
                opts.set_download_path(str(profile.download_dir))
            if port is not None:
                opts.set_local_port(port)
            else:
                opts.auto_port()

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
