# app/runtime/protocol.py
import json

class PacketBuilder:
    @staticmethod
    def make_request(name: str, payload: dict = None) -> str:
        packet = {"type": "request", "name": name, "payload": payload or {}}
        return json.dumps(packet)

    @staticmethod
    def make_response(ok: bool, data: dict = None, error: str = None) -> str:
        response = {"ok": ok}
        if data:
            response.update(data)
        if error:
            response["error"] = error
        return json.dumps(response)

    @staticmethod
    def make_command(name: str, payload: dict = None) -> str:
        packet = {"type": "command", "name": name, "payload": payload or {}}
        return json.dumps(packet)

    @staticmethod
    def make_event(name: str, payload: dict = None) -> str:
        packet = {"type": "event", "name": name, "payload": payload or {}}
        return json.dumps(packet)
