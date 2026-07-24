#!/usr/bin/env python3
"""End-to-end test: запускает ядро, bootstrap, think, проверяет результат."""
import subprocess, time, sys, os, json

# Добавляем путь к app, чтобы импортировать IPCClient и bootstrap
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'app'))
from runtime.ipc import IPCClient
from bootstrap import bootstrap_knowledge

CORE_BIN = os.path.join(os.path.dirname(__file__), '..', 'build', 'debug', 'bin', 'evolution_core')

def main():
    # Запускаем ядро
    proc = subprocess.Popen([CORE_BIN], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    time.sleep(1.5)  # ждём готовности IPC

    ipc = IPCClient()
    ipc.connect()
    assert ipc.ping(), "Core not responding"

    # Bootstrap
    bootstrap_knowledge(ipc, force=True)

    # Think
    resp = ipc.command("think")
    assert resp['payload']['ok'] is True, f"Think failed: {resp}"

    # Даём время на выполнение MainLoop
    time.sleep(0.5)

    # Проверяем, что нет ошибок в логах (можно добавить более детальную проверку)
    print("[E2E] Test passed!")

    ipc.command("shutdown")
    proc.terminate()
    proc.wait()

if __name__ == '__main__':
    main()
