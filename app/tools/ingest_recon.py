# app/tools/ingest_recon.py
import sys
from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[1]
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

from core.sdk import CoreClient
from knowledge.sensors.nmap_adapter import NmapAdapter
from knowledge.sensors.json_finding_adapter import JSONFindingAdapter

ADAPTERS = {
    "nmap":   lambda: NmapAdapter(),
    "nuclei": lambda: JSONFindingAdapter("host", ["info.name", "matched-at", "info.severity"], "nuclei"),
    "httpx":  lambda: JSONFindingAdapter("url", ["title", "webserver", "tech"], "httpx"),
}

def main():
    if len(sys.argv) < 4:
        print("Usage: ingest_recon.py <nmap|nuclei|httpx> <output_file> <scope>")
        sys.exit(1)

    tool, path, scope = sys.argv[1], sys.argv[2], sys.argv[3]
    raw = Path(path).read_text(encoding="utf-8", errors="replace")
    atoms = ADAPTERS[tool]().to_atoms(raw, scope=scope)
    if not atoms:
        print("[ingest_recon] Ничего не извлечено.")
        return

    core = CoreClient().connect()
    core.learn({"atoms": atoms})
    core.think()   # будит MainLoop -> EventClassifier обрабатывает очередь в тот же тик
    print(f"[ingest_recon] {tool}: {len(atoms)} наблюдений поставлено в очередь '{scope}'.")

if __name__ == "__main__":
    main()
