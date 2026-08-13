# app/execution/runtime_dispatcher.py
import sys

# Импортируем наши среды
from environments.web import WebEnvironment # Это ваш LightweightHttpBackend (curl_cffi)
from environments.stealth_web import StealthWebEnvironment # Это StealthBrowserBackend

class CapabilityDispatcher:
    """
    Маршрутизирует вызовы от C-ядра (через IPC) к конкретным реализациям.
    Поддерживает пул активных сред (чтобы не переоткрывать браузер на каждый клик).
    """
    def __init__(self):
        self._active_runtimes = {}

    def get_or_create_runtime(self, implementation_id: str, session_id: str):
        """Возвращает живую среду или поднимает новую."""
        cache_key = f"{implementation_id}_{session_id}"

        if cache_key in self._active_runtimes:
            return self._active_runtimes[cache_key]

        print(f"[Dispatcher] Booting new runtime: {implementation_id} for session {session_id}")

        if implementation_id == "LightweightHttpBackend":
            env = WebEnvironment(impersonate="chrome120")
            self._active_runtimes[cache_key] = env
            return env

        elif implementation_id == "StealthBrowserBackend":
            # Поднимаем undetected-chromedriver (не headless для отладки)
            env = StealthWebEnvironment(headless=False)
            self._active_runtimes[cache_key] = env
            return env

        else:
            raise ValueError(f"Unknown implementation: {implementation_id}")

    def invoke(self, payload: dict) -> dict:
        """
        Точка входа для IPC-обработчика.
        Ожидает payload вида:
        {
            "implementation": "StealthBrowserBackend",
            "session_id": "main_task_42",
            "action": "act", # или "observe"
            "affordance_id": "el_123abc", # или "navigate"
            "params": {"text": "hello"}
        }
        """
        impl_id = payload.get("implementation")
        session_id = payload.get("session_id", "default")
        action = payload.get("action")

        if not impl_id or not action:
            return {"ok": False, "error": "Missing 'implementation' or 'action' in payload"}

        try:
            env = self.get_or_create_runtime(impl_id, session_id)

            if action == "observe":
                obs = env.observe()
                return {"ok": True, "observation": obs}

            elif action == "act":
                aff_id = payload.get("affordance_id")
                params = payload.get("params", {})
                res = env.act(aff_id, params)
                return {"ok": res["success"], "result": res}

            elif action == "close":
                env.close()
                cache_key = f"{impl_id}_{session_id}"
                if cache_key in self._active_runtimes:
                    del self._active_runtimes[cache_key]
                return {"ok": True}

            else:
                return {"ok": False, "error": f"Unknown action: {action}"}

        except Exception as e:
            return {"ok": False, "error": str(e)}

    def shutdown_all(self):
        for key, env in self._active_runtimes.items():
            try:
                env.close()
            except Exception as e:
                print(f"[Dispatcher] Error closing {key}: {e}", file=sys.stderr)
        self._active_runtimes.clear()
