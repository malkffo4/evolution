# app/core/llm.py
import os
import json
import asyncio
import httpx
import requests
from tenacity import retry, stop_after_attempt, wait_exponential, retry_if_exception_type

OLLAMA_API = "http://localhost:11434/api/generate"
OPENAI_API = "https://api.openai.com/v1/chat/completions"
DEEPSEEK_API = "https://api.deepseek.com/chat/completions"
ANTHROPIC_API = "https://api.anthropic.com/v1/messages"
GEMINI_API = "https://generativelanguage.googleapis.com/v1beta/models/{model}:generateContent"

class LLMClient:
    def __init__(self, provider="ollama", model=None, api_key=None):
        self.provider = provider
        self.model = model or {
            "ollama": "qwen2.5:3b",
            "openai": "gpt-4o-mini",
            "deepseek": "deepseek-chat",
            "anthropic": "claude-3-5-sonnet-20241022",
            "gemini": "gemini-2.0-flash",
            "web_deepseek": "deepseek",
            "web_chatgpt": "chatgpt",
            "web_gemini": "gemini"
        }.get(provider, "default")

        # Берем ключ из аргументов или из переменных окружения
        self.api_key = api_key or os.getenv(f"{provider.upper()}_API_KEY")
        self._web_client = None

    def query(self, prompt: str, system: str = None, json_mode: bool = True, timeout: int = 120) -> str:
        """Синхронная обертка для обратной совместимости."""
        if self.provider.startswith("web_"):
            return self._query_web(prompt, system, json_mode)

        return asyncio.run(self.aquery(prompt, system, json_mode, timeout))

    @retry(
        wait=wait_exponential(multiplier=1, min=2, max=30),
        stop=stop_after_attempt(5),
        retry=retry_if_exception_type((httpx.HTTPError, httpx.TimeoutException))
    )
    async def aquery(self, prompt: str, system: str = None, json_mode: bool = True, timeout: int = 120) -> str:
        """Асинхронный вызов LLM с автоматическим ретраем при ошибках сети и лимитах (429)."""
        async with httpx.AsyncClient(timeout=timeout) as client:
            if self.provider == "ollama":
                return await self._aquery_ollama(client, prompt, system, json_mode)
            elif self.provider == "openai":
                return await self._aquery_openai(client, prompt, system, json_mode)
            elif self.provider == "deepseek":
                return await self._aquery_deepseek(client, prompt, system, json_mode)
            elif self.provider == "anthropic":
                return await self._aquery_anthropic(client, prompt, system, json_mode)
            elif self.provider == "gemini":
                return await self._aquery_gemini(client, prompt, system, json_mode)
            else:
                raise ValueError(f"Unknown async provider: {self.provider}")

    # --- Асинхронные провайдеры ---

    async def _aquery_ollama(self, client: httpx.AsyncClient, prompt: str, system: str, json_mode: bool) -> str:
        payload = {"model": self.model, "prompt": prompt, "stream": False}
        if system: payload["system"] = system
        if json_mode: payload["format"] = "json"

        resp = await client.post(OLLAMA_API, json=payload)
        resp.raise_for_status()
        return resp.json().get("response", "")

    async def _aquery_openai(self, client: httpx.AsyncClient, prompt: str, system: str, json_mode: bool) -> str:
        if not self.api_key: raise ValueError("OPENAI_API_KEY is missing")
        headers = {"Authorization": f"Bearer {self.api_key}", "Content-Type": "application/json"}
        messages = [{"role": "system", "content": system}] if system else []
        messages.append({"role": "user", "content": prompt})

        payload = {"model": self.model, "messages": messages, "temperature": 0.1}
        if json_mode: payload["response_format"] = {"type": "json_object"}

        resp = await client.post(OPENAI_API, headers=headers, json=payload)
        resp.raise_for_status()
        return resp.json()["choices"][0]["message"]["content"]

    async def _aquery_deepseek(self, client: httpx.AsyncClient, prompt: str, system: str, json_mode: bool) -> str:
        if not self.api_key: raise ValueError("DEEPSEEK_API_KEY is missing")
        # DeepSeek API совместим с OpenAI форматом
        headers = {"Authorization": f"Bearer {self.api_key}", "Content-Type": "application/json"}
        messages = [{"role": "system", "content": system}] if system else []
        messages.append({"role": "user", "content": prompt})

        payload = {"model": self.model, "messages": messages, "temperature": 0.1}
        if json_mode: payload["response_format"] = {"type": "json_object"}

        resp = await client.post(DEEPSEEK_API, headers=headers, json=payload)
        resp.raise_for_status()
        return resp.json()["choices"][0]["message"]["content"]

    async def _aquery_anthropic(self, client: httpx.AsyncClient, prompt: str, system: str, json_mode: bool) -> str:
        if not self.api_key: raise ValueError("ANTHROPIC_API_KEY is missing")
        headers = {
            "x-api-key": self.api_key,
            "anthropic-version": "2023-06-01",
            "content-type": "application/json"
        }
        payload = {
            "model": self.model,
            "max_tokens": 4096,
            "messages": [{"role": "user", "content": prompt}],
            "temperature": 0.1
        }
        if system: payload["system"] = system
        if json_mode:
            # Claude не имеет response_format="json_object", заставляем его выдавать JSON префиллингом
            payload["messages"].append({"role": "assistant", "content": "{"})

        resp = await client.post(ANTHROPIC_API, headers=headers, json=payload)
        resp.raise_for_status()
        content = resp.json()["content"][0]["text"]
        return "{" + content if json_mode else content

    async def _aquery_gemini(self, client: httpx.AsyncClient, prompt: str, system: str, json_mode: bool) -> str:
        if not self.api_key: raise ValueError("GEMINI_API_KEY is missing")
        url = GEMINI_API.format(model=self.model) + f"?key={self.api_key}"

        full_prompt = f"System: {system}\n\nUser: {prompt}" if system else prompt
        payload = {
            "contents": [{"parts": [{"text": full_prompt}]}],
            "generationConfig": {"temperature": 0.1}
        }
        if json_mode:
            payload["generationConfig"]["responseMimeType"] = "application/json"

        resp = await client.post(url, json=payload)
        resp.raise_for_status()
        return resp.json()["candidates"][0]["content"]["parts"][0]["text"]

    # --- Синхронный Web-провайдер ---
    def _query_web(self, prompt: str, system: str, json_mode: bool) -> str:
        if self._web_client is None:
            from core.web_llm import WebLLMClient
            self._web_client = WebLLMClient(provider=self.provider, headless=False)

        if json_mode:
            prompt += "\n\nОтветь СТРОГО в формате валидного JSON без markdown-обрамления."
        if system:
            prompt = f"Системные инструкции: {system}\n\nЗапрос: {prompt}"

        return self._web_client.query(prompt)
