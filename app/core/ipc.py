# app/core/ipc.py
import socket
import itertools
import struct
import json

# Константы (должны совпадать с C)
IPC_NAME_SIZE = 64
IPC_PAYLOAD_SIZE = 65536
IPC_FLAG_BINARY = 0x00000001

# Порядок байт: Little-Endian (= нативный для x86_64, как в C)
# Формат заголовка (offsetof(payload) в C):
# id (uint64), parent_id (uint64), timestamp (uint64),
# type (uint32), source[32], destination[32], name[64], payload_size (uint32)
HEADER_FMT = '<QQQI32s32s64sI'
HEADER_SIZE = struct.calcsize(HEADER_FMT)

FLAGS_FMT = '<I'
FLAGS_SIZE = struct.calcsize(FLAGS_FMT)

LOCK_FILE = "/tmp/evolution.lock"
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

        name = packet.get("name", "")
        payload = packet.get("payload", "")

        # Если payload – dict, сериализуем в JSON-строку
        if isinstance(payload, (dict, list)):
            payload = json.dumps(payload)

        # Если payload – строка, кодируем в байты
        if isinstance(payload, str):
            payload_bytes = payload.encode('utf-8')
        elif isinstance(payload, bytes):
            payload_bytes = payload
        else:
            payload_bytes = b''

        id = packet.get("id", 0)
        parent_id = packet.get("parent_id", 0)
        timestamp = packet.get("timestamp", 0)
        ptype = packet.get("type", 0)
        source = packet.get("source", "\\").encode('utf-8')[:32]
        destination = packet.get("destination", "").encode('utf-8')[:32]
        name_enc = name.encode('utf-8')[:64] # Если payload – строка, кодируем в байты
        if isinstance(payload, str):
            payload_bytes = payload.encode()
        elif isinstance(payload, bytes):
            payload_bytes = payload
        else:
            payload_bytes = b''

        id = packet.get("id", 0)
        parent_id = packet.get("parent_id", 0)
        timestamp = packet.get("timestamp", 0)
        ptype = packet.get("type", 0)
        source = packet.get("source", "").encode()[:32]
        destination = packet.get("destination", "").encode()[:32]
        name_enc = name.encode()[:64]

        payload_size = len(payload_bytes)
        if payload_size > IPC_PAYLOAD_SIZE:
            raise IPCError(f"Payload too large: {payload_size}")

        header = struct.pack(HEADER_FMT,
            id, parent_id, timestamp, ptype,
            source.ljust(32, b'\0'),
            destination.ljust(32, b'\0'),
            name_enc.ljust(64, b'\0'),
            payload_size)

        self.file.write(header)
        if payload_size > 0:
            self.file.write(payload_bytes)

        flags = packet.get("flags", 0)
        self.file.write(struct.pack(FLAGS_FMT, flags))
        self.file.flush()

    def _recv(self):
        if not self.file:
            raise IPCError("Not connected")

        header_data = self.file.read(HEADER_SIZE)
        if len(header_data) < HEADER_SIZE:
            raise IPCError("Connection closed")

        id, parent_id, timestamp, ptype, source, destination, name_enc, payload_size = \
            struct.unpack(HEADER_FMT, header_data)

        payload_data = b''
        if payload_size > 0:
            payload_data = self.file.read(payload_size)
            if len(payload_data) < payload_size:
                raise IPCError("Connection closed")

        flags_data = self.file.read(FLAGS_SIZE)
        if len(flags_data) < FLAGS_SIZE:
            raise IPCError("Connection closed")

        flags = struct.unpack(FLAGS_FMT, flags_data)[0]

        # Определяем тип содержимого
        if flags & IPC_FLAG_BINARY:
            payload = payload_data  # сырые байты
        else:
            try:
                payload = json.loads(payload_data.decode('utf-8'))
            except (json.JSONDecodeError, UnicodeDecodeError):
                payload = payload_data.decode('utf-8', errors='replace')

        # Определяем тип содержимого
        if flags & IPC_FLAG_BINARY:
            payload = payload_data  # сырые байты
        else:
            try:
                payload = json.loads(payload_data.decode())
            except (json.JSONDecodeError, UnicodeDecodeError):
                payload = payload_data.decode(errors='replace')

        return {
            "id": id,
            "parent_id": parent_id,
            "timestamp": timestamp,
            "type": ptype,
            "name": name_enc.rstrip(b'\0').decode(),
            "payload": payload,
            "flags": flags
        }

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
            # payload уже распарсен в _recv, если это был JSON
            payload = r.get("payload", {})
            if isinstance(payload, str):
                payload = json.loads(payload)
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
