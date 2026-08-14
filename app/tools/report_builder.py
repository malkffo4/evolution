# app/tools/report_builder.py
"""
Порядок стадий берётся ИЗ ГРАФА (PRECEDES-рёбра между известными stage),
а не из константы в коде — тот же принцип "никакого хардкода", что и в
адаптерах Задачи 2. Если методология для домена не усвоена — отчёт честно
перечисляет находки без порядка, а не выдумывает его в Python.
"""
import sys
from collections import defaultdict, deque
from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[1]
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

from core.sdk import CoreClient
from knowledge.domain_ns import namespace_entity


def topological_stage_order(core: CoreClient, stages: set[str], ordering_relation: str = "PRECEDES") -> list[str]:
    edges: dict[str, set[str]] = defaultdict(set)
    indegree: dict[str, int] = {s: 0 for s in stages}

    for stage in stages:
        for atom in core.advise(stage).get("advisories", []):
            if atom.get("process") != ordering_relation:
                continue
            args = atom.get("args", [])
            if len(args) != 2:
                continue
            a, b = args
            if a in stages and b in stages and b not in edges[a]:
                edges[a].add(b)
                indegree[b] += 1

    queue = deque(s for s in stages if indegree[s] == 0)
    order = []
    while queue:
        node = queue.popleft()
        order.append(node)
        for nxt in edges[node]:
            indegree[nxt] -= 1
            if indegree[nxt] == 0:
                queue.append(nxt)

    return order + sorted(stages - set(order))


def build_report(core: CoreClient, scope: str) -> str:
    scope_ns = namespace_entity(scope, "cybersec")
    advisories = core.advise(scope_ns).get("advisories", [])

    progress = [a for a in advisories if a.get("process") == "ATTACK_PROGRESS"]
    stages = {a["args"][1] for a in progress if len(a.get("args", [])) == 2}
    order = topological_stage_order(core, stages) if stages else []
    rank = {s: i for i, s in enumerate(order)}
    progress.sort(key=lambda a: rank.get(a["args"][1], len(order)))

    lines = [f"# Черновик отчёта: {scope}", "",
             "> Собрано автоматически. Каждый пункт требует ручной верификации перед отправкой.", ""]

    if not progress:
        lines.append("Подтверждённых стадий цепочки атаки не найдено — "
                      "либо мало наблюдений, либо онтология домена не усвоена.")
    else:
        for atom in progress:
            lines.append(f"## Стадия: {atom['args'][1]}  "
                         f"_( {atom.get('confidence_tier')}, confidence={atom.get('confidence', 0):.2f} )_\n")

    other = [a for a in advisories if a.get("process") != "ATTACK_PROGRESS"]
    if other:
        lines.append("## Прочие связанные наблюдения")
        for atom in other[:30]:
            lines.append(f"- `{atom.get('process')}` ({atom.get('confidence_tier')}, "
                         f"{atom.get('confidence', 0):.2f}): {', '.join(atom.get('args', []))}")

    return "\n".join(lines)


def main():
    if len(sys.argv) < 2:
        print("Usage: report_builder.py <scope>"); sys.exit(1)
    core = CoreClient().connect()
    print(build_report(core, sys.argv[1]))
    core.close()

if __name__ == "__main__":
    main()
