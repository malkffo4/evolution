# NeuroCore browser runtime upgrade

This bundle hardens the browser layer around the code shown in the uploaded project snapshot.

## Fixed

1. `httpx.Client(http2=True)` now has the required `h2` dependency via `httpx[http2]`.
2. Browser identity is represented by a persistent `BrowserProfile`.
3. Every active profile gets its own Chromium user-data directory and (when needed) its own local debugging port via `auto_port()`.
4. One Chromium process can own multiple independent `Tab` objects.
5. Dispatcher keys runtime state by `(session_id, tab_id)` and resolves a profile rather than silently using a global default profile.
6. URL scope checking is exact/subdomain-aware instead of a raw `endswith()` check.
7. Browser lifecycle is centralized in `BrowserFabric`.
8. The old `ChromiumPage`-centric single-tab runtime is reduced to a thin `EnvironmentRuntime` adapter.

## Deliberate boundary

This bundle implements privacy/session isolation and an authorized pentest browser sandbox. It does not implement techniques whose primary purpose is defeating a third-party WAF/anti-fraud system or guaranteeing that separate identities cannot be correlated by a remote service.

## Integration

Copy the files under `environments/` and `execution/` into the project, then merge `requirements-additions.txt` into `requirements.txt`.

The expected browser payload shape is:

```json
{
  "capability": "ObserveVisualWebState",
  "session_id": "account_a",
  "action": "act",
  "params": {
    "tab_id": "chatgpt_1",
    "profile_config": {
      "profile_id": "account_a",
      "user_data_dir": ".browser_profiles/account_a"
    },
    "affordance_id": "navigate",
    "args": {"url": "https://example.com"}
  }
}
```

NEUROCORE / DEEPSEEK WEB FIX BUNDLE

Replace these files in the real checkout:
  app/core/web_llm.py
  app/core/llm.py
  app/environments/browser/fabric.py

Included unchanged for completeness:
  app/environments/browser/profile.py

Key fixes:
1. WebLLMClient is no longer a singleton; provider switches cannot reuse a stale DeepSeek/ChatGPT profile or tab.
2. Web-chat prompt formatting is done exactly once. The system prompt and JSON-only suffix are no longer duplicated before being typed into the browser.
3. Before sending, the client snapshots the existing assistant messages. It only returns content after it observes a new/changed assistant response, so an old response cannot be mistaken for the current answer.
4. Sending prefers a real submit button and falls back to Enter.
5. No fresh response within timeout raises TimeoutError instead of silently returning an empty/old response, so LLMClient can fall back to another provider.
6. The browser profile keeps a stable per-provider user-data directory and uses --password-store=basic for portable credential/cookie storage in Linux/container environments.
7. BrowserFabric closes Chromium first so the persistent profile is flushed before process exit.
8. timeout passed to LLMClient now reaches the browser wait loop.

Validation completed here:
- Python syntax compilation passed for all fixed source files.
- Static invariants for one-time formatting, new-response tracking, submit fallback, and persistent profile passed.

Not executed here:
- Live DeepSeek login/send/response E2E, because DrissionPage is not installed in this execution environment.
