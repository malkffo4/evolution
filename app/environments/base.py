# app/environments/base.py
from abc import ABC, abstractmethod

class EnvironmentRuntime(ABC):
    @abstractmethod
    def observe(self) -> dict:
        """Возвращает текущее состояние и доступные аффордансы."""
        pass

    @abstractmethod
    def act(self, affordance: str, params: dict = None) -> dict:
        """Применяет действие и возвращает результат (успех/ошибка, новые данные)."""
        pass
