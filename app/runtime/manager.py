#!/usr/bin/env python3
import signal
import subprocess
import time
import os
from pathlib import Path

from runtime.ipc import IPCClient, DEFAULT_SOCKET


class EvolutionManager:
    def __init__(self):
        self.root = Path(__file__).resolve().parents[2]
        self.core_dir = self.root / "core"
        self.core_bin = self.root / "evolution_core"
        self.makefile = self.core_dir / "Makefile"
        self.core_process = None
        self.ipc = IPCClient()
        self.running = True
        self.core_started_by_manager = False
        self.need_shutdown = False

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
                os.unlink(DEFAULT_SOCKET)
                print("[Manager] Removed stale socket.")
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

    def run(self):
        import sys, time
        print("\nEvolution Runtime")
        print("Type exit to quit.\n")
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
                self.need_shutdown = False
                break
            elif text.lower() == "shutdown":
                try:
                    resp = self.ipc.command("shutdown")
                    print(resp)
                    time.sleep(0.5)
                except Exception as e:
                    print(f"[ERROR] {e}")
                self.need_shutdown = True
                break
            else:
                try:
                    resp = self.ipc.request("chat", {"text": text})
                    print(resp)
                except Exception as e:
                    print(f"[ERROR] {e}")

    def shutdown(self):
        print("\n[Manager] Shutdown...")
        self.running = False
        try:
            self.ipc.close()
        except Exception:
            pass
        if self.core_process and self.core_started_by_manager and self.need_shutdown:
            print("[Manager] Stopping core...")
            self.core_process.send_signal(signal.SIGTERM)
            try:
                self.core_process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.core_process.kill()
        else:
            if self.core_process and self.core_started_by_manager:
                print("[Manager] Core left running.")
        print("[Manager] Bye.")
