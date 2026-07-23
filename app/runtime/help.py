# app/runtime/help.py
# -*- coding: utf-8 -*-

GENERAL_HELP = """
╔══════════════════════════════════════════════════════════════╗
║                     NEUROCORE COMMANDS                      ║
╠══════════════════════════════════════════════════════════════╣
║  Команда          Описание                                  ║
╠══════════════════════════════════════════════════════════════╣
║  help             Показать это сообщение                    ║
║  help <command>   Справка по конкретной команде             ║
║  retrieve <q>     Найти факты в графе по ключевому слову    ║
║  learn <text>     Извлечь знания из текста через LLM        ║
║  think            Запустить один цикл MainLoop              ║
║  bootstrap        Инициализировать Meta-Core (однократно)   ║
║  shutdown         Остановить ядро и выйти                   ║
║  exit             Выйти из менеджера (ядро продолжит работу) ║
║  [любой текст]    Задать вопрос LLM (RAG по графу)          ║
╚══════════════════════════════════════════════════════════════╝
"""

RETRIEVE_HELP = """
retrieve <keyword> – Поиск фактов в графе знаний.
Примеры:
  retrieve buffer overflow
  retrieve kerberos
  retrieve smb

Ядро вернёт все известные связи, в которых участвует указанное понятие.
"""

LEARN_HELP = """
learn <text> – Извлечение знаний через LLM и сохранение в граф.
Примеры:
  learn strcpy может вызвать переполнение буфера
  learn Kerberos использует порт 88

Текст будет обработан локальной LLM, которая выделит сущности и связи,
после чего они будут загружены в LMDB.
"""

THINK_HELP = """
think – Принудительно запускает один цикл MainLoop.
Это полезно для немедленной проверки целей и алгоритмов,
не дожидаясь следующего тика демона (каждые 100 мс).
"""

BOOTSTRAP_HELP = """
bootstrap – Загружает Meta-Core и базовые алгоритмы.
Достаточно выполнить ОДИН раз после первого запуска ядра.
Повторный вызов безопасен – дубликаты не создаются.
Загружаются:
  - Мета-типы (Goal, Algorithm, Relation, SOLVES, HAS_ALGORITHM)
  - Цель FindVulnerability
  - Алгоритм CheckEdgeAlgo
"""

SHUTDOWN_HELP = """
shutdown – Корректно останавливает C-ядро и завершает менеджер.
Все данные, сохранённые в LMDB, остаются на диске.
"""

EXIT_HELP = """
exit – Выход из менеджера.
Ядро продолжает работать в фоне, его можно перезапустить позже
или подключиться к нему новым экземпляром менеджера.
"""

CHAT_HELP = """
[любой текст] – Общение с LLM, обогащённое фактами из графа.
Менеджер сначала запрашивает у ядра релевантные факты,
затем отправляет их вместе с вопросом в локальную LLM (Ollama).
"""

COMMAND_HELP = {
    "retrieve": RETRIEVE_HELP,
    "learn": LEARN_HELP,
    "think": THINK_HELP,
    "bootstrap": BOOTSTRAP_HELP,
    "shutdown": SHUTDOWN_HELP,
    "exit": EXIT_HELP,
    "chat": CHAT_HELP,
    "help": "help или help <команда> – показать справку.\n"
}

def show_help(command: str = None):
    if command is None or command not in COMMAND_HELP:
        print(GENERAL_HELP)
        return
    print(COMMAND_HELP.get(command, GENERAL_HELP))
