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
    """Persistent, isolated browser identity for one user-owned account/session.

    This class intentionally describes isolation/configuration, not WAF bypass.
    """

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

    def prepare(self) -> None:
        self.user_data_dir.mkdir(parents=True, exist_ok=True)
        if self.download_dir:
            self.download_dir.mkdir(parents=True, exist_ok=True)
        self.manifest_path.write_text(
            json.dumps(
                {
                    "profile_id": self.profile_id,
                    "locale": self.locale,
                    "timezone": self.timezone,
                    "headless": self.headless,
                    "has_proxy": bool(self.proxy),
                    "user_agent": self.user_agent,
                    "stable_seed": self.stable_seed,
                },
                ensure_ascii=False,
                indent=2,
            ),
            encoding="utf-8",
        )
