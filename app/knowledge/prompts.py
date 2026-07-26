# app/knowledge/prompts.py
"""Единственная схема экстракции знаний для всего проекта.
Заменяет: cybersec_analyzer.txt, cybersec_analyzer2.txt, analyze_with_llm.txt,
ask_ollama_for_graph.txt, test.txt — они рассинхронизированы с perception.c
и должны быть удалены."""

EXTRACTION_PROMPT = """Ты — точный экстрактор знаний. Читай ТОЛЬКО то, что явно
написано в тексте, не додумывай. Извлеки технические факты: концепции,
уязвимости (CVE, классы багов), инструменты, техники атак, команды,
причинно-следственные связи.

Игнорируй титульники, списки авторов, посвящения, оглавления.

Верни СТРОГО валидный JSON, никакого текста до/после:
{{
  "nodes": [
    {{"id": "snake_case_id", "label": "человекочитаемое имя",
      "danger": 0.0, "utility": 0.0}}
  ],
  "edges": [
    {{"source": "node_id", "target": "node_id",
      "relation": "CAUSES|USES|REQUIRES|EXPLOITS|PART_OF|LEADS_TO|ALLOWS_READING|MAY_CONTAIN"}}
  ]
}}

Если фактов нет — верни пустые списки.

Текст:
\"\"\"{chunk}\"\"\"
"""
