#!/usr/bin/env python3

import signal
import subprocess
import time
from pathlib import Path

from runtime.ipc import IPCClient


class EvolutionManager:
    def __init__(self):
        self.root = Path(__file__).resolve().parents[2]
        self.core_dir = self.root / "core"
        self.core_bin = self.root / "evolution_core"
        self.makefile = self.core_dir / "Makefile"
        self.core_process = None
        self.ipc = IPCClient()
        self.running = True

    def initialize(self):
        print("[Manager] Initializing...")

        self.check_project()
        self.build_core_if_needed()
        self.start_core()
        self.connect_ipc()
        self.wait_core()

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

        result = subprocess.run(
            ["make"],
            cwd=self.core_dir
        )

        if result.returncode != 0:
            raise RuntimeError("Core build failed.")

        if not self.core_bin.exists():
            raise RuntimeError("Compiled binary not found.")

    def start_core(self):
        print("[Manager] Starting C core...")

        self.core_process = subprocess.Popen(
            [str(self.core_bin)],
            cwd=self.core_dir,
        )

    def wait_core(self):
        print("[Manager] Waiting IPC...")

        timeout = 15
        start = time.time()
        while time.time() - start < timeout:
            if self.ipc.ping():
                print("[Manager] IPC connected.")
                return
            time.sleep(0.25)
        raise RuntimeError("IPC timeout.")

    def connect_ipc(self):
        self.ipc.connect()

    def run(self):
        print()
        print("Evolution Runtime")
        print("Type exit to quit.\n")

        while self.running:
            try:
                text = input("> ").strip()
            except EOFError:
                break

            if not text:
                continue

            if text.lower() == "exit":
                break

            response = self.ipc.request(
                "chat",
                {"text": text}
            )
            print(response)

    def shutdown(self):
        print("\n[Manager] Shutdown...")
        self.running = False
        try:
            self.ipc.close()
        except Exception:
            pass
        if self.core_process:
            self.core_process.send_signal(signal.SIGTERM)
            try:
                self.core_process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.core_process.kill()
        print("[Manager] Bye.")
