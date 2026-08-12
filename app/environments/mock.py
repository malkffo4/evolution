# app/environments/mock.py
from .base import EnvironmentRuntime

class MockEnvironment(EnvironmentRuntime):
    def __init__(self):
        self.counter = 0

    def observe(self) -> dict:
        return {
            "state": {
                "counter": self.counter
            },
            "affordances": ["increment", "decrement", "reset"]
        }

    def act(self, affordance: str, params: dict = None) -> dict:
        if affordance == "increment":
            self.counter += 1
            return {"success": True, "observation": {"counter": self.counter}}
        elif affordance == "decrement":
            self.counter -= 1
            return {"success": True, "observation": {"counter": self.counter}}
        elif affordance == "reset":
            self.counter = 0
            return {"success": True, "observation": {"counter": self.counter}}

        return {"success": False, "error": f"Unknown affordance: {affordance}"}
