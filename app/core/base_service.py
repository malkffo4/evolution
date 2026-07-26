# app/core/base_service.py
# Каждый сервис наследует BaseService, что гарантирует единый интерфейс запуска, логирование и доступ к ядру.
from abc import ABC, abstractmethod
import logging

class BaseService(ABC):
    def __init__(self, ipc_client, llm_client=None, config=None):
        self.ipc = ipc_client
        self.llm = llm_client
        self.config = config or {}
        self.logger = logging.getLogger(self.__class__.__name__)

    @abstractmethod
    def run(self, *args, **kwargs):
        """Основной метод сервиса."""
        pass

    def send_to_core(self, payload: dict):
        """Отправить знания в C-ядро."""
        import json
        resp = self.ipc.command("learn", json.dumps(payload))
        self.logger.info(f"Sent to core: {resp.get('payload', '')}")
