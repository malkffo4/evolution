import json
import socket
import itertools

DEFAULT_SOCKET = "/tmp/evolution.sock"
DEFAULT_TIMEOUT = 0.5  # таймаут на операции с сокетом

class IPCError(Exception):
    pass

class IPCClient:
    def __init__(self, socket_path=DEFAULT_SOCKET, timeout=DEFAULT_TIMEOUT):
        self.socket_path = socket_path
        self.timeout = timeout
        self.sock = None
        self.file = None
        self.ids = itertools.count(1)

    def connect(self):
        if self.sock:
            return
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)   # устанавливаем таймаут
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

    def request(self, name, **kwargs):
        packet = {"id": next(self.ids), "type": "request", "name": name, "payload": kwargs or {}}
        self._send(packet)
        return self._recv()

    def command(self, name, **kwargs):
        packet = {"id": next(self.ids), "type": "command", "name": name, "payload": kwargs or {}}
        self._send(packet)
        return self._recv()

    def event(self, name, **kwargs):
        packet = {"type": "event", "name": name, "payload": kwargs or {}}
        self._send(packet)

    def ping(self):
        try:
            r = self.request("ping")
            return r.get("ok", False)
        except Exception:
            return False

    def ping_with_timeout(self, timeout=None):
        try:
            if timeout is not None:
                # Создаём временный клиент с этим таймаутом
                client = IPCClient(socket_path=self.socket_path, timeout=timeout)
                client.connect()
                result = client.ping()
                client.close()
                return result
            else:
                return self.ping()
        except Exception:
            return False
