# app/knowledge/sensors/nmap_adapter.py
import xml.etree.ElementTree as ET
from .base import SensorAdapter

class NmapAdapter(SensorAdapter):
    """`nmap -oX -`. Один finding на (host,port): порт/сервис/версия как есть."""
    tool_name = "nmap"

    def parse(self, raw_output: str):
        root = ET.fromstring(raw_output)
        for host in root.findall("host"):
            addr_el = host.find("address")
            if addr_el is None:
                continue
            addr = addr_el.get("addr")
            ports_el = host.find("ports")
            if ports_el is None:
                continue

            for port in ports_el.findall("port"):
                state_el = port.find("state")
                if state_el is None or state_el.get("state") != "open":
                    continue
                service_el = port.find("service")
                service = service_el.get("name", "") if service_el is not None else ""
                product = service_el.get("product", "") if service_el is not None else ""
                version = service_el.get("version", "") if service_el is not None else ""
                portid, proto = port.get("portid"), port.get("protocol")

                entity = f"{addr}:{portid}/{proto}"
                yield {
                    "entity": entity,
                    "text": f"{entity} {service} {product} {version}".strip(),
                    "properties": {"host": addr, "port": int(portid), "protocol": proto,
                                    "service": service, "product": product, "version": version},
                }
