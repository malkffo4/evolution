import os
from router import command

def process_folder(self, folder_path:str):
    for root, _dirs, files in os.walk(folder_path):
        for file in files:
            pass

def process_learn_folder(path):
    facts = []
    import io, contextlib
    stdout_capture = io.StringIO()
    with contextlib.redirect_stdout(stdout_capture):
        process_folder(path)
    output = stdout_capture.getvalue()
    for line in output.splitlines():
        line = line.strip()
        if line:
            facts.append(line)
    return facts

@command("learn")
def handle(payload):
    path = payload["path"]

    if not os.path.isdir(path):
        return {
            "ok": False,
            "error": "Invalid path"
        }

    facts = process_learn_folder(path)

    return {
        "facts": facts
    }

def read(file):
    if file.endswith(".pdf"):
        pdf_path = os.path.join(root, file)
        print(f"[EYES] Читаю книгу: {pdf_path}", file=sys.stderr)
        try:
            doc = fitz.open(pdf_path)
            for page_num in range(len(doc)):
                text = doc[page_num].get_text()
                if len(text.strip()) > 50:
                    # Пробуем сначала фреймовый парсер
                    graph_json = self._ask_ollama_for_frames(text[:1500])
                    if graph_json:
                        # Преобразуем в старый формат для совместимости
                        final_json = self._frames_to_graph_json(graph_json)
                        if final_json:
                            print(final_json, flush=True)
                        else:
                            # Если преобразование не удалось, выдаём как есть (может быть старый формат)
                            print(graph_json, flush=True)
                    else:
                            # Fallback на старый метод
                            fallback = self._ask_ollama_for_graph(text[:1500])
                            if fallback:
                                print(fallback, flush=True)
        except Exception as e:
            print(f"[EYES] Ошибка чтения {file}: {e}", file=sys.stderr)
