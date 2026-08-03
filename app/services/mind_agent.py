# app/services/mind_agent.py
"""
Единый мост Understanding <-> C-ядро NeuroCore/KOSMOS.
Zero-Hardcode Mind Loop.
"""

import time
from core.sdk import CoreClient, parse_json, djb2_hash
from core.llm import LLMClient

# ---------------------------------------------------------------------
# LLM-промпты
# ---------------------------------------------------------------------
UNDERSTAND_PROMPT = """Ты — модуль Understanding когнитивного ядра NeuroCore.
Переведи запрос пользователя в СТАБИЛЬНЫЙ идентификатор задачи (Goal) и
числовые входные параметры. Одна и та же ПРИРОДА задачи (например, "среднее
трёх чисел") ВСЕГДА должна получать один и тот же goal_id — меняются только
параметры. Это критично: goal_id используется системой, чтобы находить и
переиспользовать уже изученные алгоритмы, а не синтезировать новый при
каждом обращении.

Правила:
- goal_id: snake_case, ТОЛЬКО природа задачи, без конкретных чисел из
  запроса. "среднее из 5, 10, 15" и "среднее из 100, 200, 300" -> ОДИН и тот
  же goal_id (например "average_of_three_numbers").
- params: словарь "<номер_регистра>": <целое число>. Используй регистры
  1..7 (регистр 5 зарезервирован системой — НЕ используй его). Только
  целые числа (int64). Для дробных величин укажи общий множитель scale.
- Если запрос не сводится к конкретному вычислению (открытый вопрос,
  рассуждение) — верни "kind":"reasoning" и params:{{}}.

Верни СТРОГО JSON, без пояснений:
{{
  "goal_id": "average_of_three_numbers",
  "description": "Вычислить среднее арифметическое трёх целых чисел",
  "kind": "compute",
  "params": {{"1": 5, "2": 10, "3": 15}},
  "scale": 1
}}

Запрос пользователя:
\"\"\"{text}\"\"\"
"""

SYNTHESIZE_PROMPT = """Ты — компилятор алгоритмов виртуальной машины NeuroCore.
Система искала готовый алгоритм для задачи "{description}" (goal_id={goal_id})
и не нашла его в памяти. Синтезируй НОВЫЙ алгоритм как последовательность
шагов безопасного DSL.

Доступные операции (СТРОГО только эти):
  {{"op":"load_const","dst":R,"value":N}}    R = целочисленная константа N
  {{"op":"add","dst":R,"a":R1,"b":R2}}       R = R1 + R2
  {{"op":"sub","dst":R,"a":R1,"b":R2}}       R = R1 - R2
  {{"op":"mul","dst":R,"a":R1,"b":R2}}       R = R1 * R2
  {{"op":"div","dst":R,"a":R1,"b":R2}}       R = R1 / R2  (целочисленно)
  {{"op":"move","dst":R,"src":R1}}           R = R1

Правила:
- Входные параметры УЖЕ лежат в регистрах {param_registers} — не создавай
  для них load_const, они установлены системой перед запуском.
- Регистры 8..40 — для промежуточных вычислений. 1..7 заняты входом,
  41..63 зарезервированы рантаймом — не используй их.
- Все числа целые (int64). Вход уже масштабирован scale={scale} — выведи
  результат в ТОМ ЖЕ масштабе.
- output_register — регистр с финальным результатом.

Верни СТРОГО JSON, без пояснений:
{{"steps": [...], "output_register": 0}}
"""

RENDER_PROMPT = """Ты — голос когнитивного ядра NeuroCore. Сформулируй
короткий естественный ответ пользователю на языке его вопроса. Не упоминай
регистры, VM, LMDB, байткод, Episode — пользователь не должен видеть
внутреннюю механику. Если результат отсутствует (None) — честно скажи, что
ядро выполнило задачу, но не опубликовало структурированный результат
(и предположи, что это могло быть рассуждение, а не вычисление).

Вопрос: {text}
Сырые данные, полученные от ядра: {result}
Статус выполнения: {status}
"""

class PipelineCompiler:
    def __init__(self, algo_name: str):
        self.algo_name = algo_name
        self.code = []
        self.int_consts = []
        self.str_consts = []

    def _int_const(self, value) -> int:
        idx = len(self.int_consts)
        self.int_consts.append(int(value))
        return idx

    def _str_const(self, value: str) -> int:
        idx = len(self.str_consts)
        self.str_consts.append(str(value))
        return idx

    def _emit(self, op: str, arg=()):
        a = list(arg) + [0] * (6 - len(arg))
        self.code.append({"operator_id": op, "arg": a[:6]})

    def compile(self, steps: list) -> dict:
        for s in steps:
            op = s["op"]
            if op == "load_const":
                self._emit("load_const", (s["dst"], self._int_const(s["value"])))
            elif op == "load_str":
                self._emit("load_str", (s["dst"], self._str_const(s["value"])))
            elif op in ("add", "sub", "mul", "div"):
                self._emit(op, (s["dst"], s["a"], s["b"]))
            elif op == "move":
                self._emit("move", (s["dst"], s["src"]))
            elif op == "prop_set":
                key_reg = s.get("key_reg", 58)
                self._emit("load_str", (key_reg, self._str_const(s["key"])))
                self._emit("prop_set", (s["entity_reg"], key_reg, s["value_reg"]))
            elif op == "tool_exec":
                interp_reg = s.get("interp_reg", 60)
                cmd_reg = s.get("cmd_reg", 61)
                out_reg = s.get("output_dst", 62)
                self._emit("load_str", (interp_reg, self._str_const(s.get("interpreter", "/bin/sh"))))
                self._emit("load_str", (cmd_reg, self._str_const(s["command"])))
                self._emit("tool_exec", (s["status_dst"], interp_reg, cmd_reg, out_reg))
            elif op == "halt":
                self._emit("halt")
            else:
                raise ValueError(f"unsafe or unknown DSL op: {op!r}")

        if not self.code or self.code[-1]["operator_id"] != "halt":
            self._emit("halt")

        return {
            "type": "pipeline",
            "algo_name": self.algo_name,
            "code": self.code,
            "constants": {"int_consts": self.int_consts, "str_consts": self.str_consts},
        }

class MindAgent:
    GOAL_UTILITY = 0.9
    EPISODE_TIMEOUT_EXISTING = 4.0
    EPISODE_TIMEOUT_AFTER_SYNTH = 8.0
    POLL_INTERVAL = 0.25

    def __init__(self, core_client: CoreClient, llm_client: LLMClient):
        self.core = core_client
        self.llm = llm_client

    def understand(self, text: str) -> dict:
        raw = self.llm.query(UNDERSTAND_PROMPT.format(text=text), json_mode=True)
        data = parse_json(raw) or {}
        # Используем C-совместимый djb2 для стабильности ID
        data.setdefault("goal_id", "unknown_goal_" + str(djb2_hash(text))[:8])
        data.setdefault("params", {})
        data.setdefault("scale", 1)
        return data

    def synthesize(self, goal_id: str, description: str, params: dict, scale: int) -> dict:
        prompt = SYNTHESIZE_PROMPT.format(
            description=description, goal_id=goal_id,
            param_registers=list(params.keys()), scale=scale,
        )
        raw = self.llm.query(prompt, json_mode=True)
        dsl = parse_json(raw)
        if not dsl or "steps" not in dsl:
            raise RuntimeError(f"LLM failed to synthesize a valid pipeline for '{goal_id}'")
        return dsl

    def render(self, text: str, result, status: str) -> str:
        prompt = RENDER_PROMPT.format(text=text, result=result, status=status)
        return self.llm.query(prompt, json_mode=False)

    def think(self, text: str) -> str:
        understanding = self.understand(text)
        goal_id = understanding["goal_id"]
        description = understanding.get("description", text)
        params = understanding.get("params", {})
        scale = understanding.get("scale", 1)

        self.core.activate_goal(goal_id, utility=self.GOAL_UTILITY)
        self.core.think()

        episode = self.core.wait_for_episode(goal_id, self.EPISODE_TIMEOUT_EXISTING, self.POLL_INTERVAL)
        algo_name = None

        if episode is None:
            # Алгоритма нет — синтезируем
            dsl = self.synthesize(goal_id, description, params, scale)
            algo_name = f"Synth_{goal_id}_{int(time.time())}"

            steps = list(dsl["steps"])
            out_reg = dsl.get("output_register", 0)
            if out_reg != 0:
                steps.append({"op": "move", "dst": 0, "src": out_reg})

            steps.append({"op": "load_const", "dst": 50, "value": djb2_hash(goal_id)})
            steps.append({"op": "prop_set", "entity_reg": 50, "key": "result", "value_reg": 0})

            pipeline_json = PipelineCompiler(algo_name).compile(steps)

            self.core.learn(pipeline_json)
            self.core.link_algorithm(algo_name, goal_id)
            self.core.clear_cooldown(goal_id)

            self.core.activate_goal(goal_id, utility=self.GOAL_UTILITY)
            self.core.think()

            episode = self.core.wait_for_episode(goal_id, self.EPISODE_TIMEOUT_AFTER_SYNTH, self.POLL_INTERVAL)

        if episode is None:
            return self.render(text, None, "no_algorithm_found_or_execution_failed")

        status = "success" if episode.succeeded else "failed"

        if algo_name:
            regs = self.core.exec_algorithm(algo_name, report_regs=[0])
            raw_result = regs.get("0")
            result_value = (raw_result / scale) if (raw_result is not None and scale not in (0, 1)) else raw_result
        else:
            prop = self.core.get_property(goal_id, "result")
            result_value = prop.get("value")

        return self.render(text, result_value, status)
