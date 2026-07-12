import json
import socket
import itertools

DEFAULT_SOCKET = "/tmp/evolution.sock"

class IPCError(Exception):
    pass

class IPCClient:
    def __init__(self, socket_path=DEFAULT_SOCKET):
        self.socket_path = socket_path
        self.sock = None
        self.ids = itertools.count(1)

    def connect(self):
        if self.sock:
            return

        self.sock = socket.socket(
            socket.AF_UNIX,
            socket.SOCK_STREAM
        )

        self.sock.connect(self.socket_path)

        self.file = self.sock.makefile(
            "rwb",
            buffering=0
        )

    def close(self):
        if self.sock:
            self.sock.close()
            self.sock = None

    def _send(self, packet):
        data = (
            json.dumps(packet) + "\n"
        ).encode()

        self.file.write(data)

        self.file.flush()

    def _recv(self):
        line = self.file.readline()

        if not line:
            raise IPCError(
                "Connection closed."
            )

        return json.loads(line.decode())

    def request(self, name, **kwargs):
        packet = {
            "id": next(self.ids),
            "type": "request",
            "name": name,
            "payload": kwargs or {}
        }

        self._send(packet)

        return self._recv()

    def command(self, name, **kwargs):
        packet = {
            "id": next(self.ids),
            "type": "command",
            "name": name,
            "payload": kwargs or {}
        }

        self._send(packet)

        return self._recv()

    def event(self, name, **kwargs):
        packet = {
            "type": "event",
            "name": name,
            "payload": kwargs or {}
        }

        self._send(packet)

    def ping(self):
        try:
            r = self.request("ping")
            return r.get("ok", False)
        except Exception:
            return False
