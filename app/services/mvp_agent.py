# app/services/mvp_agent.py
import json
import re

from knowledge.prompts import EXTRACTION_PROMPT

STOPWORDS = {
    "как", "что", "почему", "это", "для", "чем", "или", "если",
    "when", "what", "how", "is", "the", "a", "and", "или"
}

# Веса скоринга для отбора атомов в контекст — тюнятся под задачу
W_STI = 0.5
W_UTILITY = 0.4
W_TRUTH = 0.1
MIN_TRUTH_CONFIDENCE = 0.2  # атомы с очень низкой уверенностью в truth отсеиваем


def _extract_keywords(text: str, top_k: int = 5) -> list:
    words = re.findall(r"[a-zA-Zа-яА-Я0-9_]{3,}", text.lower())
    seen, out = set(), []
    for w in words:
        if w in STOPWORDS or w in seen:
            continue
        seen.add(w)
        out.append(w)
    return out[:top_k]


def _atom_score(atom: dict) -> float:
    """Скоринг атома для отбора в контекст LLM: sti + utility, с поправкой на truth."""
    truth = atom.get("truth", {})
    truth_mean = truth.get("mean", 1.0)
    truth_conf = truth.get("confidence", 0.5)
    if truth_conf < MIN_TRUTH_CONFIDENCE:
        return -1.0  # ненадёжный факт — не берём в контекст вообще

    attn = atom.get("attention", {})
    sti = attn.get("sti", 0.0)
    utility = atom.get("utility", 0.0)

    return W_STI * sti + W_UTILITY * utility + W_TRUTH * truth_mean


class MvpAgent:
    """80/20 цикл: текст -> NeuroAtom-триплеты с 3 когнитивными векторами (LLM) ->
    запись в C-ядро (LMDB) -> отбор контекста по STI/Utility -> ответ LLM."""

    def __init__(self, ipc_client, llm_client):
        self.ipc = ipc_client
        self.llm = llm_client

    def extract_atoms(self, text: str) -> dict:
        prompt = EXTRACTION_PROMPT.format(chunk=text[:3000])
        raw = self.llm.query(prompt, json_mode=True)
        graph = self._parse_json(raw) or {}
        graph.setdefault("atoms", [])
        return graph

    @staticmethod
    def _parse_json(raw: str):
        if not raw:
            return None
        raw = raw.strip()
        raw = re.sub(r"^```(json)?", "", raw).strip()
        raw = re.sub(r"```$", "", raw).strip()
        try:
            return json.loads(raw)
        except json.JSONDecodeError:
            pass
        m = re.search(r"\{.*\}", raw, re.DOTALL)
        if m:
            try:
                return json.loads(m.group(0))
            except json.JSONDecodeError:
                return None
        return None

    def store_atoms(self, graph: dict) -> dict:
        if not graph.get("atoms"):
            return {"ok": False, "error": "empty atom set, nothing to store"}
        return self.ipc.command("learn", json.dumps(graph))

    def retrieve_context(self, text: str, max_keywords: int = 5,
                          top_k: int = 12) -> str:
        """Тянет атомы по ключевым словам, ранжирует по score(sti, utility, truth)
        и берёт top_k — вместо линейного дампа всего, что нашлось."""
        candidates = []
        for kw in _extract_keywords(text, max_keywords):
            try:
                resp = self.ipc.request("retrieve", {"query": kw})
                payload = resp.get("payload", {})
                if isinstance(payload, str):
                    payload = json.loads(payload) if payload else {}
                atoms = payload.get("atoms", [])
            except Exception:
                continue
            candidates.extend(atoms)

        scored = [(a, _atom_score(a)) for a in candidates]
        scored = [(a, s) for a, s in scored if s >= 0]
        scored.sort(key=lambda pair: pair[1], reverse=True)

        lines = []
        for atom, score in scored[:top_k]:
            args = atom.get("args", [])
            proc = atom.get("process", "?")
            valence = atom.get("valence", 0.0)
            flag = " [РИСК]" if valence <= -0.3 else ""
            if len(args) >= 2:
                lines.append(f"{args[0]} --{proc}--> {args[1]}{flag}  (score={score:.2f})")
            elif args:
                lines.append(f"{proc}({args[0]}){flag}  (score={score:.2f})")

        return "\n".join(lines)

    def generate_reply(self, user_text: str, context: str) -> str:
        if context:
            prompt = (
                "Используй факты из графа знаний ниже (отсортированы по важности "
                "и полезности для текущей задачи). Атомы с пометкой [РИСК] требуют "
                "осторожности при использовании.\n\n"
                f"Факты:\n{context}\n\nВопрос пользователя: {user_text}"
            )
        else:
            prompt = user_text
        return self.llm.query(prompt, json_mode=False)

    def step(self, user_text: str) -> str:
        graph = self.extract_atoms(user_text)
        self.store_atoms(graph)
        context = self.retrieve_context(user_text)
        return self.generate_reply(user_text, context)
