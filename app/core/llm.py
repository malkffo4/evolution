# app/core/llm.py
import os
import json
import sys
import base64
import mimetypes
import asyncio
import httpx
import requests
from tenacity import retry, stop_after_attempt, wait_exponential, retry_if_exception, RetryError
from dotenv import load_dotenv

load_dotenv()

OLLAMA_API = "http://localhost:11434/api/generate"
OPENAI_API = "https://api.openai.com/v1/chat/completions"
DEEPSEEK_API = "https://api.deepseek.com/chat/completions"
ANTHROPIC_API = "https://api.anthropic.com/v1/messages"
GEMINI_API = "https://generativelanguage.googleapis.com/v1beta/models/{model}:generateContent"

LLM_CACHE_FILE = ".working_llm_cache.json"

def _is_retryable_error(e):
    if isinstance(e, httpx.HTTPStatusError):
        status = e.response.status_code
        if status == 429:
            try:
                error_data = e.response.json()
                err_type = error_data.get("error", {}).get("type", "")
                err_code = error_data.get("error", {}).get("code", "")
                if "insufficient_quota" in (err_type, err_code) or \
                   "credit_balance_exhausted" in (err_type, err_code) or \
                   "billing_hard_limit_reached" in (err_type, err_code):
                    return False
            except Exception:
                pass
            return True
        if status in (401, 402, 403):
            return False
        return status in (500, 502, 503, 504)
    if isinstance(e, httpx.RequestError):
        return True
    return False

class LLMClient:
    def __init__(self, provider="auto", model=None, api_key=None):
        self._web_client = None
        self.providers = []

        if provider == "auto" or provider == "ollama":
            if provider == "ollama":
                try:
                    requests.get("http://localhost:11434/", timeout=0.5)
                    self.providers.append("ollama")
                except requests.RequestException:
                    pass

            def is_valid_key(k):
                if not k: return False
                if "YOUR_KEY" in k or "OTHER_KEY" in k: return False
                try:
                    k.encode('ascii')
                    return True
                except UnicodeEncodeError:
                    return False

            if is_valid_key(os.getenv("DEEPSEEK_API_KEY")): self.providers.append("deepseek")
            if is_valid_key(os.getenv("GEMINI_API_KEY")): self.providers.append("gemini")
            if is_valid_key(os.getenv("OPENAI_API_KEY")): self.providers.append("openai")
            if is_valid_key(os.getenv("ANTHROPIC_API_KEY")): self.providers.append("anthropic")

            if "ollama" not in self.providers:
                try:
                    if requests.get("http://localhost:11434/", timeout=0.5).status_code == 200:
                        self.providers.append("ollama")
                except requests.RequestException:
                    pass

            # Всегда добавляем веб-провайдеры как последний рубеж на случай
            # исчерпания лимитов API.
            self.providers.append("web_deepseek")
            self.providers.append("web_chatgpt")

            if not self.providers:
                print("[LLM] API ключей нет, использую локальную Ollama.", file=sys.stderr)
                self.providers.append("ollama")

            self.provider = self.providers[0]
        else:
            self.provider = provider
            self.providers = [provider]

        self.model = model
        self.api_key = api_key
        self.model_to_use = None
        self.key_to_use = None

        # 1. Читаем кэш прямо при инициализации, чтобы сразу подхватить рабочий LLM
        if os.path.exists(LLM_CACHE_FILE):
            try:
                with open(LLM_CACHE_FILE, "r") as f:
                    cached = json.load(f).get("provider")
                    if cached and cached in self.providers:
                        self.providers.remove(cached)
                        self.providers.insert(0, cached)
                        self.provider = cached
            except Exception:
                pass

        # 2. Сразу инициализируем переменные, иначе прямые вызовы aquery() упадут
        self.model_to_use = self._get_model(self.provider)
        self.key_to_use = self._get_api_key(self.provider)

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

    def _encode_image(self, image_path: str) -> str:
        with open(image_path, "rb") as image_file:
            return base64.b64encode(image_file.read()).decode('utf-8')

    def _get_mime_type(self, image_path: str) -> str:
        mime_type, _ = mimetypes.guess_type(image_path)
        return mime_type or "image/jpeg"

    def query(self, prompt: str, system: str = None, json_mode: bool = True, timeout: int = 120, image_path: str = None) -> str:
        """Синхронная обертка с автоматическим каскадным фоллбэком и поддержкой Vision."""

        # 1. Читаем кэш и двигаем рабочий провайдер на 1 место
        if os.path.exists(LLM_CACHE_FILE):
            try:
                with open(LLM_CACHE_FILE, "r") as f:
                    cached = json.load(f).get("provider")
                    if cached and cached in self.providers:
                        self.providers.remove(cached)
                        self.providers.insert(0, cached)
            except Exception:
                pass

        for i, provider in enumerate(self.providers):
            self.provider = provider
            self.model_to_use = self._get_model(provider)
            self.key_to_use = self._get_api_key(provider)

            if provider.startswith("web_"):
                try:
                    res = self._query_web(prompt, system, json_mode, image_path)
                    # Фиксируем успех
                    with open(LLM_CACHE_FILE, "w") as f: json.dump({"provider": provider}, f)
                    return res
                except Exception as e:
                    err_msg = str(e)
                    short_err = err_msg[:120] + ("... [TRUNCATED]" if len(err_msg) > 120 else "")
                    print(f"\n[LLM] Ошибка web-генерации ({provider}): {short_err}", file=sys.stderr)
                    if os.path.exists(LLM_CACHE_FILE): os.remove(LLM_CACHE_FILE)
                    if i < len(self.providers) - 1:
                        print(f"[LLM] Переключаюсь на следующий провайдер: {self.providers[i+1]}...", file=sys.stderr)
                    continue

            try:
                try:
                    loop = asyncio.get_running_loop()
                except RuntimeError:
                    loop = None

                if loop and loop.is_running():
                    import threading
                    res_box = []
                    err_box = []
                    def _run():
                        try:
                            res_box.append(asyncio.run(self.aquery(prompt, system, json_mode, timeout, image_path)))
                        except Exception as ex:
                            err_box.append(ex)
                    t = threading.Thread(target=_run)
                    t.start()
                    t.join()
                    if err_box:
                        raise err_box[0]
                    res = res_box[0]
                else:
                    res = asyncio.run(self.aquery(prompt, system, json_mode, timeout, image_path))

                # Фиксируем успех
                with open(LLM_CACHE_FILE, "w") as f: json.dump({"provider": provider}, f)
                return res

            except Exception as e:
                err_msg = str(e)
                if isinstance(e, RetryError):
                    e = e.last_attempt.exception()
                    err_msg = str(e)
                if isinstance(e, httpx.HTTPStatusError):
                    err_msg = f"HTTP {e.response.status_code} - {e.response.text}"

                # 2. ГАСИМ ПРОСТЫНЮ ЗДЕСЬ
                short_err = err_msg[:120] + ("... [TRUNCATED]" if len(err_msg) > 120 else "")
                print(f"\n[LLM] Ошибка генерации ({provider}): {short_err}", file=sys.stderr)
                if os.path.exists(LLM_CACHE_FILE): os.remove(LLM_CACHE_FILE)

                if i < len(self.providers) - 1:
                    print(f"[LLM] Переключаюсь на следующий провайдер: {self.providers[i+1]}...", file=sys.stderr)
                continue

        return "{}" if json_mode else "Произошла ошибка связи со всеми LLM провайдерами."

    @retry(
        wait=wait_exponential(multiplier=1, min=2, max=30),
        stop=stop_after_attempt(5),
        retry=retry_if_exception(_is_retryable_error)
    )
    async def aquery(self, prompt: str, system: str = None, json_mode: bool = True, timeout: int = 120, image_path: str = None) -> str:
        async with httpx.AsyncClient(timeout=timeout) as client:
            if self.provider == "ollama":
                return await self._aquery_ollama(client, prompt, system, json_mode, image_path)
            elif self.provider == "openai":
                return await self._aquery_openai(client, prompt, system, json_mode, image_path)
            elif self.provider == "deepseek":
                return await self._aquery_deepseek(client, prompt, system, json_mode, image_path)
            elif self.provider == "anthropic":
                return await self._aquery_anthropic(client, prompt, system, json_mode, image_path)
            elif self.provider == "gemini":
                return await self._aquery_gemini(client, prompt, system, json_mode, image_path)
            else:
                raise ValueError(f"Unknown async provider: {self.provider}")

    async def _aquery_ollama(self, client: httpx.AsyncClient, prompt: str, system: str, json_mode: bool, image_path: str) -> str:
        payload = {"model": self.model_to_use, "prompt": prompt, "stream": False}
        if system: payload["system"] = system
        if json_mode: payload["format"] = "json"
        if image_path: payload["images"] = [self._encode_image(image_path)]
        resp = await client.post(OLLAMA_API, json=payload)
        resp.raise_for_status()
        return resp.json().get("response", "")

    async def _aquery_openai(self, client: httpx.AsyncClient, prompt: str, system: str, json_mode: bool, image_path: str) -> str:
        if not self.key_to_use: raise ValueError("OPENAI_API_KEY is missing")
        headers = {"Authorization": f"Bearer {self.key_to_use}", "Content-Type": "application/json"}
        messages = [{"role": "system", "content": system}] if system else []

        if image_path:
            base64_img = self._encode_image(image_path)
            mime_type = self._get_mime_type(image_path)
            messages.append({
                "role": "user",
                "content": [
                    {"type": "text", "text": prompt},
                    {"type": "image_url", "image_url": {"url": f"data:{mime_type};base64,{base64_img}"}}
                ]
            })
        else:
            messages.append({"role": "user", "content": prompt})

        payload = {"model": self.model_to_use, "messages": messages, "temperature": 0.1}
        if json_mode: payload["response_format"] = {"type": "json_object"}
        resp = await client.post(OPENAI_API, headers=headers, json=payload)
        resp.raise_for_status()
        return resp.json()["choices"][0]["message"]["content"]

    async def _aquery_deepseek(self, client: httpx.AsyncClient, prompt: str, system: str, json_mode: bool, image_path: str) -> str:
        if not self.key_to_use: raise ValueError("DEEPSEEK_API_KEY is missing")
        if image_path:
            raise ValueError("DeepSeek API does not support vision yet. Forcing fallback to next provider.")

        headers = {"Authorization": f"Bearer {self.key_to_use}", "Content-Type": "application/json"}
        messages = [{"role": "system", "content": system}] if system else []
        messages.append({"role": "user", "content": prompt})

        payload = {"model": self.model_to_use, "messages": messages, "temperature": 0.1}
        if json_mode: payload["response_format"] = {"type": "json_object"}
        resp = await client.post(DEEPSEEK_API, headers=headers, json=payload)
        resp.raise_for_status()
        return resp.json()["choices"][0]["message"]["content"]

    async def _aquery_anthropic(self, client: httpx.AsyncClient, prompt: str, system: str, json_mode: bool, image_path: str) -> str:
        if not self.key_to_use: raise ValueError("ANTHROPIC_API_KEY is missing")
        headers = {"x-api-key": self.key_to_use, "anthropic-version": "2023-06-01", "content-type": "application/json"}

        user_content = []
        if image_path:
            user_content.append({
                "type": "image",
                "source": {
                    "type": "base64",
                    "media_type": self._get_mime_type(image_path),
                    "data": self._encode_image(image_path)
                }
            })
        user_content.append({"type": "text", "text": prompt})

        payload = {
            "model": self.model_to_use,
            "max_tokens": 4096,
            "messages": [{"role": "user", "content": user_content}],
            "temperature": 0.1
        }
        if system: payload["system"] = system
        if json_mode: payload["messages"].append({"role": "assistant", "content": "{"})

        resp = await client.post(ANTHROPIC_API, headers=headers, json=payload)
        resp.raise_for_status()
        content = resp.json()["content"][0]["text"]
        return "{" + content if json_mode else content

    async def _aquery_gemini(self, client: httpx.AsyncClient, prompt: str, system: str, json_mode: bool, image_path: str) -> str:
        if not self.key_to_use: raise ValueError("GEMINI_API_KEY is missing")
        url = GEMINI_API.format(model=self.model_to_use) + f"?key={self.key_to_use}"

        parts = []
        if system: parts.append({"text": f"System: {system}\n\n"})
        parts.append({"text": f"User: {prompt}"})

        if image_path:
            parts.append({
                "inlineData": {
                    "mimeType": self._get_mime_type(image_path),
                    "data": self._encode_image(image_path)
                }
            })

        payload = {
            "contents": [{"parts": parts}],
            "generationConfig": {"temperature": 0.1}
        }
        if json_mode: payload["generationConfig"]["responseMimeType"] = "application/json"

        resp = await client.post(url, json=payload)
        resp.raise_for_status()
        return resp.json()["candidates"][0]["content"]["parts"][0]["text"]

    def _query_web(self, prompt: str, system: str, json_mode: bool, image_path: str) -> str:
        if self._web_client is None:
            from core.web_llm import WebLLMClient
            self._web_client = WebLLMClient(provider=self.provider, headless=False)

        if json_mode: prompt += "\n\nОтветь СТРОГО в формате валидного JSON без markdown-обрамления."
        if system: prompt = f"Системные инструкции: {system}\n\nЗапрос: {prompt}"

        return self._web_client.query(prompt, image_path)
