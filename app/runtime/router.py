@request("ping")
def ping(_):
    return {"ok": True}

class Registry:
    def __init__(self):
        self._tables = {
            "request": {}, 
            "command": {}, 
            "event": {},
            }

    def handler(self, packet_type: str, name: str):
        if packet_type not in self._tables:
            raise ValueError(f"Unknown packet type: {packet_type}")
        
        table = self._tables[packet_type]

        if name in table:
            raise RuntimeError(f"{packet_type}:{name} already registered")

        def wrapper(func):
            self._tables[packet_type][name] = func
            return func
        
        return wrapper

    def get(self, packet_type: str, name: str):
        table = self._tables.get(packet_type)
        if table is None:
            return None
        return table.get(name)

    def unregister(self, packet_type: str, name: str):
        table = self._tables.get(packet_type)
        if table is not None:
            table.pop(name, None)


registry = Registry()

def request(name):
    return registry.handler("request", name)

def command(name):
    return registry.handler("command", name)

def event(name):
    return registry.handler("event", name)

import services

def dispatch(packet):
    packet_type = packet.get("type")
    name = packet.get("name")
    payload = packet.get("payload", {})

    handler = registry.get(packet_type, name)
    if handler is None:
        return {"ok": False, "error": f"Unknown {packet_type}: {name}"}

    try:
        result = handler(payload) or {}

        if "ok" not in result:
            result["ok"] = True
        return result
    except Exception as e:
        return {"ok": False, "error": str(e)}