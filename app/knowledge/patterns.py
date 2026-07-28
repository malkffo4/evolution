# app/knowledge/patterns.py
import json

def build_chat_relevant_facts_pattern() -> dict:
    """Placeholder semantics: matches any IS_A fact.
    Replace conditions once you decide what 'relevant to the chat' means —
    e.g. match on entities that appear as args in this session's EVENT_TYPE atoms."""
    return {
        "type": "hyper_pattern",
        "pattern_id": 1,
        "vars": ["subject", "category"],
        "conditions": [
            {"process": "IS_A", "args": [{"var": "subject"}, {"var": "category"}]}
        ]
    }

def install_patterns(ipc):
    resp = ipc.command("learn", json.dumps(build_chat_relevant_facts_pattern()))
    if resp.get('name') == 'error':
        print(f"[Patterns] Not installed pattern 1: {resp.get('payload')}")
    else:
        print(f"[Patterns] installed pattern 1: {resp.get('payload')}")
