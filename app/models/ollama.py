import requests, json

class Local_LLM:
    def __init__(self, api=OLLAMA_API, model=MODEL1):
        self.model = model
        self.api = api

    # --- Новый метод: запрос фреймов ---
    def _ask_ollama_for_frames(self, text_chunk:str):
        """
        Просит LLM вернуть JSON с фреймами: события, роли, типы узлов.
        """
        prompt = open('prompts/cybersec_analyzer.txt', 'r').read()

        payload = {
            "model": self.model,
            "format": "json",
            "stream": False,
            "messages": [{"role": "user", "content": prompt}]
        }
        try:
            resp = requests.post(self.api, json=payload, timeout=120)
            if resp.status_code != 200:
                print(f"[LLM] HTTP {resp.status_code}: {resp.text}", file=sys.stderr)
                return None
            data = resp.json()
            content = data.get("message", {}).get("content")
            if not content:
                print(f"[LLM] No content in response: {data}", file=sys.stderr)
                return None
            return content
        except requests.exceptions.JSONDecodeError:
            print(f"[LLM] Response is not JSON: {resp.text[:200]}", file=sys.stderr)
            return None
        except requests.exceptions.Timeout:
            print(f"[LLM] Request timed out after 120s", file=sys.stderr)
            return None
        except Exception as e:
            print(f"[LLM] Request failed: {e}", file=sys.stderr)
            return None

    # --- Преобразование фреймов в старый формат (nodes/edges) для совместимости ---
    def _frames_to_graph_json(self, frames_data: dict) -> str:
        """
        Принимает ответ от _ask_ollama_for_frames и возвращает JSON строку в формате
        { "nodes": [...], "edges": [...] }, где для каждого фрейма создан узел-событие,
        а роли связаны с ним рёбрами. Также все узлы добавляются в общий список.
        """
        if not frames_data or "frames" not in frames_data:
            return None

        nodes = {}
        edges = []

        # Сначала собираем все узлы из глобального списка (если есть) и из ролей
        for node in frames_data.get("nodes", []):
            nodes[node["id"]] = node

        for frame in frames_data["frames"]:
            event_id = frame["event_id"]
            event_label = frame["event_label"]
            # Создаём узел-событие, если его ещё нет
            event_node = {
                "id": event_id,
                "label": event_label,
                "type": "EVENT",
                "danger": 0.5,  # агрегируем позже, если нужно
                "utility": 0.5
            }
            nodes[event_id] = event_node

            # Обрабатываем роли
            for role_name, role_node in frame["roles"].items():
                role_id = role_node["id"]
                if role_id not in nodes:
                    nodes[role_id] = role_node
                # Добавляем ребро от события к роли
                relation = f"HAS_{role_name}"  # HAS_AGENT, HAS_TECHNIQUE и т.д.
                edges.append({
                    "source": event_id,
                    "target": role_id,
                    "relation": relation
                })

        # Преобразуем словарь узлов обратно в список
        nodes_list = list(nodes.values())

        result = {
            "nodes": nodes_list,
            "edges": edges
        }
        return json.dumps(result)

    # --- Старый метод для обратной совместимости ---
    def _ask_ollama_for_graph(self, text_chunk:str):
        # ... оставь без изменений
        prompt = f"""
        Ты - когнитивный экстрактор. Прочитай текст и извлеки знания в формате JSON.
        Для каждого узла определи его "эмоциональный" когнитивный заряд (от 0.0 до 1.0):
        - "danger": насколько это опасно/разрушительно (например, эксплойты, ошибки = 0.9).
        - "utility": насколько это полезный инструмент (скрипты, утилиты, методы = 0.9).
        - "novelty": насколько это редкий или новый концепт.
        Текст: {text_chunk}

        Верни СТРОГО валидный JSON:
        {{
        "nodes": [
            {{"id":"...", "label":"...", "danger": 0.8, "utility": 0.2, "novelty": 0.5}}
        ],
        "edges": [
            {{"source":"...", "target":"...", "relation":"..."}}
        ]
        }}
        """
        payload = {
            "model": self.model,
            "format": "json",
            "stream": False,
            "messages": [{"role": "user", "content": prompt}]
        }
        try:
            resp = requests.post(self.api, json=payload, timeout=120)
            data = resp.json()
            content = data.get("message", {}).get("content")
            if not content:
                print(f"[LLM] No content in response: {data}", file=sys.stderr)
                return None
            return content
        except requests.exceptions.JSONDecodeError:
            print(f"[LLM] Response is not JSON: {resp.text[:200]}", file=sys.stderr)
            return None
        except requests.exceptions.Timeout:
            print(f"[LLM] Request timed out after 120s", file=sys.stderr)
            return None
        except Exception as e:
            print(f"[LLM] Request failed: {e}", file=sys.stderr)
            return None

    # --- Основной обработчик папки ---



    def _analyze_with_llm(self, word, text):
        prompt = f"""
        Проанализируй текст и извлеки свойства и связи для понятия '{word}'.
        Текст: {text}

        Верни СТРОГО JSON:
        {{
        "tokens": [
            {{"word": "{word}", "pos": "NOUN", "lemma": "{word}"}}
        ],
        "relations": [
            {{"source": "{word}", "target": "...", "type": "является"}}
        ]
        }}
        """
        payload = {
            "model": "qwen2.5-coder:1.5b",
            "format": "json",
            "stream": False,
            "messages": [{"role": "user", "content": prompt}]
        }
        try:
            resp = requests.post(self.api, json=payload, timeout=120)
            data = resp.json()
            content = data.get("message", {}).get("content")
            if not content:
                print(f"[LLM] No content in response: {data}", file=sys.stderr)
                return None
            return content
        except requests.exceptions.JSONDecodeError:
            print(f"[LLM] Response is not JSON: {resp.text[:200]}", file=sys.stderr)
            return None
        except requests.exceptions.Timeout:
            print(f"[LLM] Request timed out after 120s", file=sys.stderr)
            return None
        except Exception as e:
            print(f"[LLM] Request failed: {e}", file=sys.stderr)
            return None
