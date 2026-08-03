#!/usr/bin/env python3
# app/services/event_bus.py

import json
import sys
import threading
from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[1]
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

from core.ipc import IPCClient, TYPE_COMMAND

class EventBusListener:
    """
    Фоновый слушатель событий от C-ядра.
    """
    def __init__(self):
        self.ipc = IPCClient()
        self.running = False
        self.thread = None

    def start(self):
        self.ipc.connect()
        # Подписываемся на события (отправляем команду subscribe)
        packet = {"id": 999999, "type": TYPE_COMMAND, "name": "subscribe", "payload": {}}
        self.ipc._send(packet)
        resp = self.ipc._recv()
        if not resp.get("payload", {}).get("ok"):
            raise RuntimeError("Failed to subscribe to Core events")

        self.running = True
        self.thread = threading.Thread(target=self._listen_loop, daemon=True)
        self.thread.start()
        print("[EventBus] 🟢 Подключено. Ожидаю потоки мыслей от ядра...\n")

    def _listen_loop(self):
        while self.running:
            try:
                event = self.ipc._recv()
                name = event.get("name")
                payload = event.get("payload")
                print(f"⚡ [Событие Ядра] {name} -> {payload}")
            except Exception as e:
                if self.running:
                    print(f"\n[EventBus] Соединение разорвано: {e}")
                break

    def stop(self):
        self.running = False
        self.ipc.close()

if __name__ == "__main__":
    bus = EventBusListener()
    bus.start()
    try:
        import time
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        bus.stop()
