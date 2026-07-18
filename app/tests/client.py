#!/usr/sbin/env python3

from ipc import IPCClient

def maa():
    ipc = IPCClient()

    ipc.connect()

    ipc.request("chat", {"text": "hello"})

    ipc.command("learn", {"path": "/books"})

    ipc.event("memory_changed")

    ipc.close()