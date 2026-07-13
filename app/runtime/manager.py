#!/usr/bin/env python3
import signal
import subprocess
import time
import os
import sys
import json
from pathlib import Path
from runtime.ipc import IPCClient, DEFAULT_SOCKET

class EvolutionManager:
    def __init__(self):
        self.root = Path(__file__).resolve().parents[2]
        self.core_dir = self.root / "core"
        self.core_bin = self.core_dir / "evolution_core"
        self.makefile = self.core_dir / "Makefile"
        self.core_process = None
        self.ipc = IPCClient()
        self.running = True
        self.core_started_by_manager = False

    def initialize(self):
        print("[Manager] Initializing...")
        self.check_project()
        self.build_core_if_needed()

        if self.is_core_running():
            print("[Manager] Core already running, reusing existing instance.")
            self.core_started_by_manager = False
        else:
            self.start_core()
            self.core_started_by_manager = True

        self.connect_ipc()

        if self.core_started_by_manager:
            self.wait_core()
        else:
            if not self.ipc.ping():
                raise RuntimeError("Core is not responding.")
        print("[Manager] Ready.")

    def check_project(self):
        if not self.makefile.exists():
            raise RuntimeError("core/Makefile not found.")
        if not self.core_dir.exists():
            raise RuntimeError("core directory not found.")

    def build_core_if_needed(self):
        if self.core_bin.exists():
            return
        print("[Manager] Building C core...")
        result = subprocess.run(["make"], cwd=self.core_dir)
        if result.returncode != 0:
            raise RuntimeError("Core build failed.")
        if not self.core_bin.exists():
            raise RuntimeError("Compiled binary not found.")

    def start_core(self):
        if os.path.exists(DEFAULT_SOCKET):
            try:
                test = IPCClient(timeout=0.3)
                test.connect()
                test.ping()
                test.close()
            except Exception:
                try:
                    os.unlink(DEFAULT_SOCKET)
                    print("[Manager] Removed stale socket.")
                except Exception:
                    pass

        print("[Manager] Starting C core...")
        self.core_process = subprocess.Popen(
            [str(self.core_bin)],
            cwd=self.core_dir,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        time.sleep(1.0)
        self.core_started_by_manager = True

    def is_core_running(self):
        try:
            if not os.path.exists(DEFAULT_SOCKET):
                return False
            test = IPCClient(timeout=0.3)
            test.connect()
            ok = test.ping()
            test.close()
            return ok
        except Exception:
            return False

    def wait_core(self, attempts=20, interval=0.5):
        print("[Manager] Waiting IPC...")
        for attempt in range(attempts):
            if self.core_process and self.core_process.poll() is not None:
                stdout, stderr = self.core_process.communicate()
                raise RuntimeError(f"Core died: {stderr.decode()}")
            if os.path.exists(DEFAULT_SOCKET) and self.ipc.ping():
                print("[Manager] IPC connected.")
                return
            time.sleep(interval)
        raise RuntimeError("IPC timeout.")

    def connect_ipc(self):
        if self.ipc.sock is None:
            self.ipc.connect()

    def format_and_print_response(self, response):
        """Парсит полученный IPCPacket и красиво выводит его пользователю"""
        if not response:
            print("[System] No response received.")
            return

        # Если ядро вернуло строковый JSON в поле payload
        payload_raw = response.get("payload", "")
        if isinstance(payload_raw, str) and payload_raw.strip():
            try:
                # Пытаемся распарсить внутренний payload
                payload = json.loads(payload_raw)
                if isinstance(payload, dict):
                    # Если в payload есть поле 'reply' (из чата)
                    if "reply" in payload:
                        print(f"\nAI: {payload['reply']}")
                        return
                    # Если команда завершилась успешно
                    if payload.get("ok") is True:
                        print("\n[OK] Command completed successfully.")
                        return
            except json.JSONDecodeError:
                pass

        # Резервный красивый вывод на случай, если там простой текст
        if isinstance(payload_raw, str) and payload_raw.strip():
            print(f"\nAI: {payload_raw}")
        else:
            print(f"\nAI (Raw): {response}")

    def run(self):
        print("\nEvolution Runtime")
        print("Type 'exit' or 'shutdown' to quit.\n")
        while self.running:
            try:
                line = sys.stdin.buffer.readline()
                if not line:
                    break
                text = line.decode('utf-8', errors='ignore').strip()
            except (EOFError, KeyboardInterrupt):
                break

            if not text:
                continue

            if text.lower() == "exit":
                break
            elif text.lower() == "shutdown":
                try:
                    resp = self.ipc.command("shutdown")
                    self.format_and_print_response(resp)
                    time.sleep(0.5)
                except Exception as e:
                    print(f"[ERROR] {e}")
                break
            else:
                try:
                    resp = self.ipc.request("chat", {"text": text})
                    self.format_and_print_response(resp)
                except Exception as e:
                    print(f"[ERROR] {e}")

    def shutdown(self):
        print("\n[Manager] Shutdown sequence initiated...")
        self.running = False
        try:
            self.ipc.close()
        except Exception:
            pass

        # КРИТИЧЕСКОЕ ИСПРАВЛЕНИЕ: Если менеджер САМ запустил ядро, он обязан его убить!
        if self.core_started_by_manager:
            if self.core_process and self.core_process.poll() is None:
                print("[Manager] Stopping core process...")
                try:
                    self.core_process.send_signal(signal.SIGTERM)
                    self.core_process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    print("[Manager] Core process ignored SIGTERM. Escalating to SIGKILL...")
                    self.core_process.kill()
                    self.core_process.wait()
            else:
                print("[Manager] Core process was already terminated.")
        else:
            print("[Manager] Core process was not started by this manager instance. Left running.")

        # Удаляем Unix-сокет, чтобы не оставлять мусор
        if self.core_started_by_manager and os.path.exists(DEFAULT_SOCKET):
            try:
                os.unlink(DEFAULT_SOCKET)
                print("[Manager] Cleaned up socket file.")
            except Exception:
                pass

        print("[Manager] Bye.")
