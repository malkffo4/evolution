# app/core/llm.py
import os
import json
import requests

OLLAMA_API = "http://localhost:11434/api/generate"
OPENAI_API = "https://api.openai.com/v1/chat/completions"
GEMINI_API = "https://generativelanguage.googleapis.com/v1beta/models/{model}:generateContent"

class LLMClient:
    def __init__(self, provider="ollama", model=None, api_key=None):
        self.provider = provider
        self.model = model or {
            "ollama": "qwen2.5:3b",
            "openai": "gpt-4o-mini",
            "gemini": "gemini-2.0-flash"
        }[provider]
        self.api_key = api_key
        self._cache = {}

    def query(self, prompt, system=None, json_mode=True, timeout=120):
        if self.provider == "ollama":
            return self._query_ollama(prompt, system, json_mode, timeout)
        elif self.provider == "openai":
            return self._query_openai(prompt, system, json_mode, timeout)
        elif self.provider == "gemini":
            return self._query_gemini(prompt, system, json_mode, timeout)
        raise ValueError(f"Unknown provider: {self.provider}")

    def _query_ollama(self, prompt, system, json_mode, timeout):
        payload = {
            "model": self.model,
            "prompt": prompt,
            "stream": False
        }
        if system:
            payload["system"] = system
        if json_mode:
            payload["format"] = "json"
        resp = requests.post(OLLAMA_API, json=payload, timeout=timeout)
        resp.raise_for_status()
        return resp.json().get("response", "")

    def _query_openai(self, prompt, system, json_mode, timeout):
        headers = {
            "Authorization": f"Bearer {self.api_key}",
            "Content-Type": "application/json"
        }
        messages = []
        if system:
            messages.append({"role": "system", "content": system})
        messages.append({"role": "user", "content": prompt})
        payload = {
            "model": self.model,
            "messages": messages,
            "temperature": 0.1
        }
        if json_mode:
            payload["response_format"] = {"type": "json_object"}
        resp = requests.post(OPENAI_API, headers=headers, json=payload, timeout=timeout)
        resp.raise_for_status()
        return resp.json()["choices"][0]["message"]["content"]

    def _query_gemini(self, prompt, system, json_mode, timeout):
        key = self.api_key or os.getenv("GEMINI_API_KEY")
        url = GEMINI_API.format(model=self.model) + f"?key={key}"
        payload = {
            "contents": [{"parts": [{"text": prompt}]}],
            "generationConfig": {"temperature": 0.1}
        }
        if json_mode:
            payload["generationConfig"]["responseMimeType"] = "application/json"
        resp = requests.post(url, json=payload, timeout=timeout)
        resp.raise_for_status()
        data = resp.json()
        return data["candidates"][0]["content"]["parts"][0]["text"]
