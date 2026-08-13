from __future__ import annotations

from dataclasses import dataclass, field
from ipaddress import ip_address
from urllib.parse import urlparse


@dataclass(frozen=True)
class TargetScope:
    """Positive authorization scope for browser-based security testing."""

    exact_hosts: frozenset[str] = field(default_factory=frozenset)
    subdomain_roots: frozenset[str] = field(default_factory=frozenset)
    allowed_ports: frozenset[int] = field(default_factory=lambda: frozenset({80, 443}))
    allowed_schemes: frozenset[str] = field(default_factory=lambda: frozenset({"http", "https"}))

    def allows(self, url: str) -> bool:
        try:
            p = urlparse(url)
            if p.scheme not in self.allowed_schemes:
                return False
            host = (p.hostname or "").rstrip(".").lower()
            if not host:
                return False
            if p.port is not None and p.port not in self.allowed_ports:
                return False
            if host in self.exact_hosts:
                return True
            return any(host.endswith("." + root.lstrip(".")) for root in self.subdomain_roots)
        except ValueError:
            return False


def validate_url_in_scope(url: str, scope: TargetScope) -> None:
    if not scope.allows(url):
        raise PermissionError(f"target is outside authorized scope: {url}")
