import json
import socket
import itertools

DEFAULT_SOCKET = "/tmp/evolution.sock"
DEFAULT_TIMEOUT = 1.0

TYPE_REQUEST = 0
TYPE_RESPONSE = 1
TYPE_COMMAND = 2
TYPE_EVENT = 3

class IPCError(Exception):
    pass

class IPCClient:
    def __init__(self, socket_path=DEFAULT_SOCKET, timeout=2.0):
        self.socket_path = socket_path
        self.timeout = timeout
        self.sock = None
        self.file = None
        self.ids = itertools.count(1)

    def connect(self):
        if self.sock:
            return
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect(self.socket_path)
        self.file = self.sock.makefile("rwb", buffering=0)

    def close(self):
        if self.file:
            self.file.close()
            self.file = None
        if self.sock:
            self.sock.close()
            self.sock = None

    def _send(self, packet):
        if not self.file:
            raise IPCError("Not connected")
        # data = (json.dumps(packet) + "\n").encode()
        # self.file.write(data)
        # self.file.flush()
        if "payload" in packet and isinstance(packet["payload"], dict):
            packet["payload"] = json.dumps(packet["payload"])

        data = (json.dumps(packet) + "\n").encode()
        self.file.write(data)
        self.file.flush()

    def _recv(self):
        if not self.file:
            raise IPCError("Not connected")
        line = self.file.readline()
        if not line:
            raise IPCError("Connection closed")
        return json.loads(line.decode())

    def request(self, name, payload=None):
        packet = {"id": next(self.ids), "type": TYPE_REQUEST, "name": name, "payload": payload or {}}
        self._send(packet)
        return self._recv()

    def command(self, name, payload=None):
        packet = {"id": next(self.ids), "type": TYPE_COMMAND, "name": name, "payload": payload or {}}
        self._send(packet)
        return self._recv()

    def event(self, name, **kwargs):
        packet = {"type": TYPE_EVENT, "name": name, "payload": kwargs or {}}
        self._send(packet)

    def ping(self):
        try:
            r = self.request("ping")
            import json
            payload = json.loads(r.get("payload", "{}"))
            return payload.get("ok", False)
        except Exception:
            return False

    def ping_with_timeout(self, timeout=None):
        if timeout is not None:
            old_timeout = self.sock.gettimeout() if self.sock else None
            if self.sock:
                self.sock.settimeout(timeout)
        try:
            return self.ping()
        finally:
            if timeout is not None and old_timeout is not None:
                self.sock.settimeout(old_timeout)
