import re
import json
import sys

def parse_system_instruction(text: str) -> dict:
    """
    Разбирает входящие предложения на русском/английском языках
    и строит детерминированный фрейм действия для передачи в C-ядро.
    """
    cleaned = text.lower().strip()

    # Шаблоны для создания папок
    mkdir_patterns = [
        r"(?:создай|сделай)\s+(?:папку|директорию|каталог)\s+(?:с\s+именем|)?\s*['\"«]?([a-zA-Z0-9_\-]+)['\"»]?",
        r"mkdir\s+([a-zA-Z0-9_\-]+)"
    ]

    for pattern in mkdir_patterns:
        match = re.search(pattern, cleaned)
        if match:
            folder_name = match.group(1)
            return {
                "nodes": [
                    {"id": f"dir_{folder_name}", "label": folder_name, "type": "DIRECTORY", "danger": 0.0, "utility": 0.8},
                    {"id": "cmd_mkdir", "label": f"mkdir {folder_name}", "type": "TOOL", "danger": 0.0, "utility": 0.9}
                ],
                "edges": [
                    {"source": "cmd_mkdir", "target": f"dir_{folder_name}", "relation": "CREATES"}
                ]
            }

    # Шаблоны для удаления
    rmdir_patterns = [
        r"(?:удали|стереть)\s+(?:папку|директорию|каталог)\s+([a-zA-Z0-9_\-]+)",
        r"rmdir\s+([a-zA-Z0-9_\-]+)"
    ]

    for pattern in rmdir_patterns:
        match = re.search(pattern, cleaned)
        if match:
            folder_name = match.group(1)
            return {
                "nodes": [
                    {"id": f"dir_{folder_name}", "label": folder_name, "type": "DIRECTORY", "danger": 0.2, "utility": 0.5},
                    {"id": "cmd_rmdir", "label": f"rmdir {folder_name}", "type": "TOOL", "danger": 0.1, "utility": 0.6}
                ],
                "edges": [
                    {"source": "cmd_rmdir", "target": f"dir_{folder_name}", "relation": "DELETES"}
                ]
            }

    # Если шаблон не совпал, возвращаем пустую структуру (сработает триггер UNKNOWN)
    return {"nodes": [], "edges": []}

if __name__ == "__main__":
    # Тестовый запуск
    test_phrase = "Создай папку с именем target_folder"
    if len(sys.argv) > 1:
        test_phrase = " ".join(sys.argv[1:])

    result = parse_system_instruction(test_phrase)
    print(json.dumps(result, ensure_ascii=False, indent=2))
