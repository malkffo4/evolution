# app/execution/runtime_dispatcher.py (Концепт)
from environments.web import WebEnvironment
from environments.mock import MockEnvironment

class EnvironmentDispatcher:
    def __init__(self):
        self._active_envs = {}

    def invoke(self, implementation_id: str, action: str, params: dict):
        if implementation_id == "PlaywrightWebRuntime":
            if "web_main" not in self._active_envs:
                self._active_envs["web_main"] = WebEnvironment(headless=False)

            env = self._active_envs["web_main"]
            if action == "observe":
                return env.observe()
            elif action == "act":
                return env.act(params["affordance_id"], params.get("args"))
