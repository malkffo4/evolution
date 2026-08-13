# app/core/bootstrap.py
import re
import json
import sys
import struct
from pathlib import Path

sys.path.append(str(Path(__file__).resolve().parents[1]))

from core.ipc import IPCClient
from knowledge.patterns import install_patterns
from core.sdk import djb2_hash

PROC_KIND_RELATION = 0
PROC_TYPE_SHIFT = 56
PROC_ID_MASK = (~(0xFF << PROC_TYPE_SHIFT)) & 0xFFFFFFFFFFFFFFFF

def proc_make(base_id: int, kind: int) -> int:
    return (base_id & PROC_ID_MASK) | ((kind & 0xFF) << PROC_TYPE_SHIFT)

def float_to_uint32(f: float) -> int:
    return struct.unpack('<I', struct.pack('<f', f))[0]

def get_opcodes_map():
    opcode_path = Path(__file__).resolve().parents[2] / "core" / "src" / "runtime" / "ops" / "opcode.h"
    op_map = {}
    if not opcode_path.exists():
        return {"OP_GLOAD_CONST": 79, "OP_ASSERT": 46}

    try:
        content = opcode_path.read_text(encoding="utf-8")
        content = re.sub(r'//.*', '', content)
        content = re.sub(r'/\*.*?\*/', '', content, flags=re.DOTALL)

        match = re.search(r'typedef\s+enum\s*\{(.*?)\}', content, re.DOTALL)
        if match:
            enum_body = match.group(1)
            counter = 0
            for line in enum_body.split(','):
                part = line.strip()
                if not part: continue

                if '=' in part:
                    name, val = part.split('=')
                    counter = int(val.strip())
                    op_map[name.strip()] = counter
                else:
                    op_map[part] = counter
                counter += 1
    except Exception:
        return {"OP_GLOAD_CONST": 79, "OP_ASSERT": 46}

    return op_map

OPCODES = get_opcodes_map()

def resolve_macros(obj):
    """Рекурсивно обходит JSON и заменяет макросы на вычисленные значения."""
    if isinstance(obj, dict):
        return {k: resolve_macros(v) for k, v in obj.items()}
    elif isinstance(obj, list):
        return [resolve_macros(v) for v in obj]
    elif isinstance(obj, str):
        if obj.startswith("@hash:"):
            return str(djb2_hash(obj.split(":", 1)[1]))
        elif obj.startswith("@proc_rel:"):
            base_hash = djb2_hash(obj.split(":", 1)[1])
            return str(proc_make(base_hash, PROC_KIND_RELATION))
        elif obj.startswith("@float:"):
            return float_to_uint32(float(obj.split(":", 1)[1]))
        elif obj.startswith("@opcode:"):
            op_name = obj.split(":", 1)[1].upper()
            if not op_name.startswith("OP_"):
                op_name = "OP_" + op_name
            return OPCODES.get(op_name, 0)
    return obj

def is_already_bootstrapped(ipc: IPCClient) -> bool:
    resp = ipc.request("retrieve", {"query": "MetaType"})
    try:
        payload = json.loads(resp.get("payload", "{}"))
        return len(payload.get("nodes", [])) > 0 or len(payload.get("atoms", [])) > 0
    except Exception:
        return False

def bootstrap_knowledge(ipc: IPCClient, force=False):
    if not force and is_already_bootstrapped(ipc):
        print("[Bootstrap] Knowledge already loaded, skipping.")
        return

    seed_dir = Path(__file__).resolve().parents[1] / "knowledge" / "seed"
    if not seed_dir.exists():
        print(f"[Bootstrap] WARN: Seed directory not found at {seed_dir}")
        return

    print("[Bootstrap] Loading initial knowledge base from JSON seeds...")

    for filepath in sorted(seed_dir.glob("*.json")):
        print(f"  -> {filepath.name}")
        try:
            raw_data = json.loads(filepath.read_text(encoding="utf-8"))

            if isinstance(raw_data, list):
                for item in raw_data:
                    processed = resolve_macros(item)
                    ipc.command("learn", json.dumps(processed))
            else:
                processed = resolve_macros(raw_data)
                ipc.command("learn", json.dumps(processed))

        except Exception as e:
            print(f"[Bootstrap] ERROR loading {filepath.name}: {e}")

    install_patterns(ipc)
    print("[Bootstrap] Complete.")
