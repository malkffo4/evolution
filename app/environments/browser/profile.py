from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import hashlib
import json
import re

from typing import Optional


_SAFE_ID = re.compile(r"^[A-Za-z0-9._-]{1,96}$")


@dataclass(frozen=True)
class BrowserProfile:
    """Persistent, isolated browser identity for a user-owned account/session."""

    profile_id: str
    user_data_dir: Path
    user_agent: Optional[str] = None
    locale: str = "en-US"
    timezone: str = "UTC"
    headless: bool = False
    proxy: Optional[str] = None
    download_dir: Optional[Path] = None

    def __post_init__(self) -> None:
        if not _SAFE_ID.fullmatch(self.profile_id):
            raise ValueError(f"Invalid profile_id: {self.profile_id!r}")
        object.__setattr__(self, "user_data_dir", Path(self.user_data_dir).expanduser().resolve())
        if self.download_dir is not None:
            object.__setattr__(self, "download_dir", Path(self.download_dir).expanduser().resolve())

    @property
    def stable_seed(self) -> int:
        digest = hashlib.sha256(self.profile_id.encode("utf-8")).digest()
        return int.from_bytes(digest[:8], "big", signed=False)

    @property
    def manifest_path(self) -> Path:
        return self.user_data_dir / "neurocore-profile.json"

    @property
    def debug_port(self) -> int:
        # Stable per-profile port so a new Python process can reconnect to
        # the same still-running Chromium instance.
        return 18000 + (self.stable_seed % 30000)

    def prepare(self) -> None:
        self.user_data_dir.mkdir(parents=True, exist_ok=True)
        if self.download_dir:
            self.download_dir.mkdir(parents=True, exist_ok=True)

        manifest = {
            "profile_id": self.profile_id,
            "user_data_dir": str(self.user_data_dir),
            "locale": self.locale,
            "timezone": self.timezone,
            "headless": self.headless,
            "has_proxy": bool(self.proxy),
            "user_agent": self.user_agent,
            "stable_seed": self.stable_seed,
            "debug_port": self.debug_port,
        }

        # Do not remove or recreate the user-data directory here. Chromium owns
        # cookies, Local Storage and session databases inside it. Only write a
        # small metadata file next to them.
        tmp = self.manifest_path.with_suffix(".tmp")
        tmp.write_text(
            json.dumps(manifest, ensure_ascii=False, indent=2),
            encoding="utf-8",
        )
        tmp.replace(self.manifest_path)
