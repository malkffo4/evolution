# app/core/llm.py
import os
import sys
import json
import asyncio
import httpx
import requests
from tenacity import retry, stop_after_attempt, wait_exponential, retry_if_exception, RetryError
from dotenv import load_dotenv

# Загружаем ключи из .env файла
load_dotenv()

OLLAMA_API = "http://localhost:11434/api/generate"
OPENAI_API = "https://api.openai.com/v1/chat/completions"
DEEPSEEK_API = "https://api.deepseek.com/chat/completions"
ANTHROPIC_API = "https://api.anthropic.com/v1/messages"
GEMINI_API = "https://generativelanguage.googleapis.com/v1beta/models/{model}:generateContent"

def _is_retryable_error(e):
    if isinstance(e, httpx.HTTPStatusError):
        status = e.response.status_code
        # Если это 429, проверяем, не закончились ли деньги
        if status == 429:
            try:
                error_data = e.response.json()
                err_type = error_data.get("error", {}).get("type", "")
                err_code = error_data.get("error", {}).get("code", "")
                # Если сработал любой триггер нехватки средств - ретраить бесполезно
                if "insufficient_quota" in (err_type, err_code) or \
                   "credit_balance_exhausted" in (err_type, err_code) or \
                   "billing_hard_limit_reached" in (err_type, err_code):
                    return False
            except Exception:
                pass
            return True # Rate limit (retryable)

        # 401 Unauthorized, 402 Payment Required (DeepSeek), 403 Forbidden - не ретраим
        if status in (401, 402, 403):
            return False

        # Повторяем только при ошибках сервера (5xx)
        return status in (500, 502, 503, 504)

    if isinstance(e, httpx.RequestError):
        return True # network errors are retryable
    return False

class LLMClient:
    def __init__(self, provider="auto", model=None, api_key=None):
        self._web_client = None
        self.providers = []

        # Если провайдер явно не задан (или передан старый дефолт "ollama"),
        # пытаемся автоматически определить лучший доступный и строим очередь фоллбэка.
        if provider == "auto" or provider == "ollama":
            if provider == "ollama":
                try:
                    requests.get("http://localhost:11434/", timeout=0.5)
                    self.providers.append("ollama")
                except requests.RequestException:
                    pass

            def is_valid_key(k):
                if not k: return False
                if "ваш_ключ" in k or "другой_ключ" in k: return False
                try:
                    k.encode('ascii')
                    return True
                except UnicodeEncodeError:
                    return False

            # Проверяем облачные ключи по приоритету
            if is_valid_key(os.getenv("DEEPSEEK_API_KEY")): self.providers.append("deepseek")
            if is_valid_key(os.getenv("GEMINI_API_KEY")): self.providers.append("gemini")
            if is_valid_key(os.getenv("OPENAI_API_KEY")): self.providers.append("openai")
            if is_valid_key(os.getenv("ANTHROPIC_API_KEY")): self.providers.append("anthropic")

            # Проверяем локальную Ollama, если она не была первой
            if "ollama" not in self.providers:
                try:
                    if requests.get("http://localhost:11434/", timeout=0.5).status_code == 200:
                        self.providers.append("ollama")
                except requests.RequestException:
                    pass

            # Фолбэк на Web LLM если вообще ничего нет
            if not self.providers:
                print("[LLM] ⚠️ Облачные ключи не найдены, Ollama не отвечает. Переход на web_deepseek.")
                self.providers.append("web_deepseek")

            self.provider = self.providers[0]
        else:
            self.provider = provider
            self.providers = [provider]

        self.model = model
        self.api_key = api_key

        # Переменные для хранения текущего контекста исполнения
        self.model_to_use = None
        self.key_to_use = None

    def _get_api_key(self, provider):
        if self.api_key: return self.api_key
        if provider == "openai": return os.getenv("OPENAI_API_KEY")
        if provider == "deepseek": return os.getenv("DEEPSEEK_API_KEY")
        if provider == "anthropic": return os.getenv("ANTHROPIC_API_KEY")
        if provider == "gemini": return os.getenv("GEMINI_API_KEY")
        return None

    def _get_model(self, provider):
        if self.model: return self.model
        default_models = {
            "ollama": "qwen2.5:3b",
            "openai": "gpt-4o-mini",
            "deepseek": "deepseek-chat",
            "anthropic": "claude-3-5-sonnet-20241022",
            "gemini": "gemini-2.0-flash",
            "web_deepseek": "deepseek",
            "web_chatgpt": "chatgpt",
            "web_gemini": "gemini"
        }
        return default_models.get(provider, "default")

    def query(self, prompt: str, system: str = None, json_mode: bool = True, timeout: int = 120) -> str:
        """Синхронная обертка с автоматическим каскадным фоллбэком между провайдерами."""
        for i, provider in enumerate(self.providers):
            self.provider = provider
            self.model_to_use = self._get_model(provider)
            self.key_to_use = self._get_api_key(provider)

            if provider.startswith("web_"):
                try:
                    return self._query_web(prompt, system, json_mode)
                except Exception as e:
                    print(f"\n[LLM] ⚠️ Ошибка web-генерации ({provider}): {e}", file=sys.stderr)
                    if i < len(self.providers) - 1:
                        print(f"[LLM] 🔄 Переключаюсь на следующий провайдер: {self.providers[i+1]}...", file=sys.stderr)
                    continue

            try:
                return asyncio.run(self.aquery(prompt, system, json_mode, timeout))
            except Exception as e:
                err_msg = str(e)
                if isinstance(e, RetryError):
                    # Вытаскиваем реальную ошибку из tenacity
                    e = e.last_attempt.exception()
                    err_msg = str(e)
                if isinstance(e, httpx.HTTPStatusError):
                    err_msg = f"HTTP {e.response.status_code} - {e.response.text}"

                print(f"\n[LLM] ⚠️ Ошибка генерации ({provider}): {err_msg}", file=sys.stderr)

                if i < len(self.providers) - 1:
                    print(f"[LLM] 🔄 Переключаюсь на следующий провайдер: {self.providers[i+1]}...", file=sys.stderr)
                    continue

        return "{}" if json_mode else "Произошла ошибка связи со всеми LLM провайдерами."

    @retry(
        wait=wait_exponential(multiplier=1, min=2, max=30),
        stop=stop_after_attempt(5),
        retry=retry_if_exception(_is_retryable_error)
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

    async def _aquery_ollama(self, client: httpx.AsyncClient, prompt: str, system: str, json_mode: bool) -> str:
        payload = {"model": self.model_to_use, "prompt": prompt, "stream": False}
        if system: payload["system"] = system
        if json_mode: payload["format"] = "json"
        resp = await client.post(OLLAMA_API, json=payload)
        resp.raise_for_status()
        return resp.json().get("response", "")

    async def _aquery_openai(self, client: httpx.AsyncClient, prompt: str, system: str, json_mode: bool) -> str:
        if not self.key_to_use: raise ValueError("OPENAI_API_KEY is missing")
        headers = {"Authorization": f"Bearer {self.key_to_use}", "Content-Type": "application/json"}
        messages = [{"role": "system", "content": system}] if system else []
        messages.append({"role": "user", "content": prompt})
        payload = {"model": self.model_to_use, "messages": messages, "temperature": 0.1}
        if json_mode: payload["response_format"] = {"type": "json_object"}
        resp = await client.post(OPENAI_API, headers=headers, json=payload)
        resp.raise_for_status()
        return resp.json()["choices"][0]["message"]["content"]

    async def _aquery_deepseek(self, client: httpx.AsyncClient, prompt: str, system: str, json_mode: bool) -> str:
        if not self.key_to_use: raise ValueError("DEEPSEEK_API_KEY is missing")
        headers = {"Authorization": f"Bearer {self.key_to_use}", "Content-Type": "application/json"}
        messages = [{"role": "system", "content": system}] if system else []
        messages.append({"role": "user", "content": prompt})
        payload = {"model": self.model_to_use, "messages": messages, "temperature": 0.1}
        if json_mode: payload["response_format"] = {"type": "json_object"}
        resp = await client.post(DEEPSEEK_API, headers=headers, json=payload)
        resp.raise_for_status()
        return resp.json()["choices"][0]["message"]["content"]

    async def _aquery_anthropic(self, client: httpx.AsyncClient, prompt: str, system: str, json_mode: bool) -> str:
        if not self.key_to_use: raise ValueError("ANTHROPIC_API_KEY is missing")
        headers = {
            "x-api-key": self.key_to_use,
            "anthropic-version": "2023-06-01",
            "content-type": "application/json"
        }
        payload = {
            "model": self.model_to_use,
            "max_tokens": 4096,
            "messages": [{"role": "user", "content": prompt}],
            "temperature": 0.1
        }
        if system: payload["system"] = system
        if json_mode:
            payload["messages"].append({"role": "assistant", "content": "{"})
        resp = await client.post(ANTHROPIC_API, headers=headers, json=payload)
        resp.raise_for_status()
        content = resp.json()["content"][0]["text"]
        return "{" + content if json_mode else content

    async def _aquery_gemini(self, client: httpx.AsyncClient, prompt: str, system: str, json_mode: bool) -> str:
        if not self.key_to_use: raise ValueError("GEMINI_API_KEY is missing")
        url = GEMINI_API.format(model=self.model_to_use) + f"?key={self.key_to_use}"
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
