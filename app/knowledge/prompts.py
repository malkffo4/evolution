# app/knowledge/prompts.py

# маленькие модели плохо держат в голове длинный список полей (truth, attention, utility, valence, kind...) одновременно с самим NER.
# Добавь few-shot прямо в EXTRACTION_PROMPT и вставь {FEW_SHOT_EXAMPLE} перед "Текст:\n\"\"\"{chunk}\"\"\"" в шаблоне.
# Few-shot почти всегда даёт маленьким моделям больший прирост качества, чем инструкции.
FEW_SHOT_EXAMPLE = """
Пример (на другом тексте, для формата):
Текст: "Функция strcpy не проверяет длину строки, что приводит к переполнению
буфера. Для защиты используйте strncpy."
Ответ:
{{"atoms": [
  {{"process": "IS_A", "kind": "relation", "args": ["strcpy", "Function"], "truth": {{"mean": 1.0, "confidence": 0.9}}, "attention": {{"sti": 0.6, "lti": 0.4}}, "utility": 0.5, "valence": -0.2}},
  {{"process": "CAUSES", "kind": "relation", "args": ["strcpy", "BufferOverflow"], "truth": {{"mean": 0.95, "confidence": 0.8}}, "attention": {{"sti": 0.7, "lti": 0.4}}, "utility": 0.6, "valence": -0.5}},
  {{"process": "MITIGATES", "kind": "relation", "args": ["strncpy", "BufferOverflow"], "truth": {{"mean": 0.9, "confidence": 0.7}}, "attention": {{"sti": 0.6, "lti": 0.4}}, "utility": 0.7, "valence": 0.4}}
]}}
"""

EXTRACTION_PROMPT = """Ты — когнитивный экстрактор знаний NeuroCore. Читай ТОЛЬКО то, что
явно написано или логически прямо следует из текста. Извлекай факты как атомы
триплетов (process, args) и для КАЖДОГО факта оцени три когнитивных вектора.

1. TRUTH (эпистемика): насколько это утверждение истинно.
   - mean: 0.0..1.0 (1.0 = точно верно, 0.5 = неопределённо, 0.0 = ложно)
   - confidence: 0.0..1.0 (насколько ты уверен в самой оценке mean)

2. ATTENTION (внимание):
   - sti (Short-Term Importance): насколько это релевантно ИМЕННО СЕЙЧАС,
     в контексте текущего разговора/задачи пользователя. 0.0..1.0
   - lti (Long-Term Importance): насколько это фундаментальный, долгоживущий
     факт, который стоит помнить надолго (а не разовая деталь). 0.0..1.0

3. UTILITY/VALENCE (телеология и аффект):
   - utility: насколько этот факт полезен для достижения целей пользователя
     (если пользователь строит базу данных — факты про БД/индексы/схемы
     получают высокий utility). 0.0..1.0
   - valence: "эмоциональный" знак факта с точки зрения безопасности/пользы
     системы. Позитивный (0.3..1.0) — конструктивно и безопасно.
     Отрицательный (-1.0..-0.3) — риск, уязвимость, разрушительное действие.
     Около нуля — нейтрально.

Также укажи "kind" узла-отношения: concept | rule | goal | event | hypothesis
(если не уверен — не указывай, будет relation по умолчанию).

Игнорируй титульники, списки авторов, оглавления.

Верни СТРОГО валидный JSON, никакого текста до/после:
{{
  "atoms": [
    {{
      "id": "опционально_snake_case_id",
      "process": "USES|CAUSES|REQUIRES|EXPLOITS|PART_OF|LEADS_TO|IS_A|...",
      "kind": "relation",
      "args": ["subject_id", "object_id"],
      "truth": {{"mean": 0.9, "confidence": 0.7}},
      "attention": {{"sti": 0.8, "lti": 0.3}},
      "utility": 0.7,
      "valence": 0.4,
      "context": "опциональный_id_контекста",
      "cause": "опциональный_id_причины"
    }}
  ]
}}

Если фактов нет — верни {{"atoms": []}}.
""" + FEW_SHOT_EXAMPLE + """
Текст:
\"\"\"{chunk}\"\"\"
"""

PRIME_EXPLICATION_PROMPT = """Ты — семантический толкователь. Тебе дано новое
понятие. Разложи его смысл через уже известные примитивы: {known_primes}.

Понятие: "{concept}"

Верни атомы вида:
EXPLICATED_AS(concept, "someone Want something because...")
COMPOSED_OF(concept, prime1)
COMPOSED_OF(concept, prime2)
IS_A(concept, category_among_known_primitives_or_concepts)
"""
