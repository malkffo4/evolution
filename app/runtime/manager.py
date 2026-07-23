#!/usr/bin/env python3
# app/runtime/manager.py

import signal, subprocess, time, os, sys, json
from pathlib import Path
from runtime.ipc import IPCClient, DEFAULT_SOCKET, LOCK_FILE
from runtime.help import show_help

class EvolutionManager:
    def __init__(self):
        self.root = Path(__file__).resolve().parents[2]
        self.core_dir = self.root / "core"
        self.core_bin = self.core_dir / "build" / "debug" / "bin" / "evolution_core"
        self.makefile = self.core_dir / "Makefile"
        self.core_process = None
        self.ipc = IPCClient()
        self.running = True
        self.core_started_by_manager = False

        # Добавляем файл для логов ядра, чтобы не ловить deadlock на пайпах
        self.core_log_path = "/tmp/evolution_core.log"
        self.core_log_fd = None

    def initialize(self):
        print("[Manager] Initializing...")
        self.check_project()
        self.build_core_if_needed()

        # Пытаемся подключиться к уже работающему ядру
        if self.is_core_responding():
            print("[Manager] Core already running, reusing existing instance.")
            self.core_started_by_manager = False
            self.connect_ipc()
            print("[Manager] Ready.")
            return

        # Ядро не отвечает – запускаем сами
        self.start_core()
        self.core_started_by_manager = True
        self.wait_core()
        print("[Manager] Ready.")

    def is_core_responding(self):
        """Живо ли ядро – проверяем только штатным пингом, без мусорных коннектов."""
        if not os.path.exists(DEFAULT_SOCKET):
            return False

        try:
            test = IPCClient(timeout=2.0)
            test.connect()
            ok = test.ping()
            test.close()
            return ok
        except Exception:
            return False

    def wait_core(self, attempts=30, interval=0.5):
        print("[Manager] Waiting IPC...")
        for _ in range(attempts):
            # Проверяем, не упало ли ядро
            if self.core_process and self.core_process.poll() is not None:
                err_msg = "Check /tmp/evolution_core.log for details."
                if self.core_log_fd:
                    self.core_log_fd.flush()
                    with open(self.core_log_path, "r") as f:
                        content = f.read().strip()
                        if content:
                            err_msg = content[-2000:] # Последние 2000 символов лога
                raise RuntimeError(f"Core died! Last logs:\n{err_msg}")

            try:
                # Пытаемся подключиться основным клиентом
                self.connect_ipc()
                # Если пинг прошел — всё отлично
                if self.ipc.ping():
                    print("[Manager] IPC connected.")
                    return
            except Exception:
                # Если ядро еще не подняло сокет, закрываем мусор и ждем
                self.ipc.close()
                self.ipc.sock = None

            time.sleep(interval)

        raise RuntimeError(f"IPC timeout. Core might be stuck. Check {self.core_log_path}")

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
        print("[Manager] Starting C core...")

        # 1. Принудительно завершаем зомби процессы
        try:
            subprocess.run(
                ["pkill", "-9", "-f", self.core_bin.name],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL
            )
            time.sleep(0.5)
        except Exception:
            pass

        # 2. Очищаем оставшиеся файлы сокета
        for stale_file in [DEFAULT_SOCKET, LOCK_FILE]:
            if os.path.exists(stale_file):
                try:
                    os.unlink(stale_file)
                    print(f"[Manager] Cleaned up stale file: {stale_file}")
                except OSError:
                    pass

        # 3. Запускаем чистый процесс ядра, вывод кидаем в файл!
        self.core_log_fd = open(self.core_log_path, "w+")
        self.core_process = subprocess.Popen(
            [str(self.core_bin)],
            cwd=self.core_dir,
            stdout=self.core_log_fd,
            stderr=subprocess.STDOUT, # Ошибки тоже идут в этот лог
        )
        time.sleep(1.5)

    def is_core_running(self):
        return self.is_core_responding()

    def connect_ipc(self):
        if self.ipc.sock is None:
            self.ipc.connect()

    def format_and_print_response(self, response):
        if not response:
            print("[System] No response received.")
            return
        payload_raw = response.get("payload", "")
        if isinstance(payload_raw, str) and payload_raw.strip():
            try:
                payload = json.loads(payload_raw)
                if isinstance(payload, dict):
                    if "reply" in payload:
                        print(f"\nAI: {payload['reply']}")
                        return
                    if payload.get("ok") is True:
                        print("\n[OK] Command completed successfully.")
                        return
            except json.JSONDecodeError:
                pass
        if isinstance(payload_raw, str) and payload_raw.strip():
            print(f"\nAI: {payload_raw}")
        else:
            print(f"\nAI (Raw): {response}")

    def run(self):
        print("\n" + "="*60)
        print("  NeuroCore Runtime")
        print("  Type 'help' for commands, 'exit' to quit")
        print("="*60 + "\n")

        while self.running:
            try:
                user_input = input("> ").strip()
            except (EOFError, KeyboardInterrupt):
                print("\n")
                break

            if not user_input:
                continue

            text = user_input
            if text.lower() == "help" or text == '?':
                show_help()
                continue
            if text.lower().startswith("help "):
                _, cmd = text.split(" ", 1)
                show_help(cmd.strip().lower())
                continue
            if text.lower() == "exit":
                break
            elif text.lower() == "shutdown":
                try:
                    resp = self.ipc.command("shutdown")
                    self.format_and_print_response(resp)
                    time.sleep(0.5)
                except Exception as e:
                    print(f"\n[ERROR] {e}")
                break
            elif text.lower().startswith("retrieve"):
                keyword = text.split(" ", 1)[1] if " " in text else text
                try:
                    resp = self.ipc.request("retrieve", {"query": keyword.lower()})
                    self.format_and_print_response(resp)
                except Exception as e:
                    print(f"\n[ERROR] {e}")
            elif text.lower().startswith("learn"):
                text_to_learn = text[6:].strip()
                if not text_to_learn:
                    print("\nUsage: learn <text to extract knowledge from>")
                    continue
                from models.learner import CognitiveLearner
                learner = CognitiveLearner()
                print("\n[Learner] Processing...")
                learner.learn_text(text_to_learn)
                print("[Learner] Done.")
            elif text.lower() == "think":
                try:
                    resp = self.ipc.command("think")
                    self.format_and_print_response(resp)
                except Exception as e:
                    print(f"\n[ERROR] {e}")
            elif text.lower() == "bootstrap":
                from bootstrap import bootstrap_knowledge
                bootstrap_knowledge(self.ipc, force=True)
                print("\n[Manager] Bootstrap complete.")
            else:
                try:
                    resp = self.ipc.request("chat", {"text": text})
                    self.format_and_print_response(resp)
                except Exception as e:
                    print(f"\n[ERROR] {e}")

    def shutdown(self):
        print("\n[Manager] Shutdown sequence initiated...")
        self.running = False
        try:
            self.ipc.close()
        except Exception:
            pass

        if self.core_started_by_manager:
            if self.core_process and self.core_process.poll() is None:
                print("[Manager] Stopping core process...")
                try:
                    self.core_process.send_signal(signal.SIGTERM)
                    self.core_process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    self.core_process.kill()
                    self.core_process.wait()
            else:
                print("[Manager] Core process was already terminated.")
            if os.path.exists(DEFAULT_SOCKET):
                try:
                    os.unlink(DEFAULT_SOCKET)
                    print("[Manager] Cleaned up socket file.")
                except Exception:
                    pass
        else:
            print("[Manager] Core process was not started by this manager instance. Left running.")

        # Закрываем файл логов
        if getattr(self, 'core_log_fd', None):
            try:
                self.core_log_fd.close()
            except:
                pass

        print("[Manager] Bye.")
