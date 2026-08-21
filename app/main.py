#!/usr/bin/env python3
# app/main.py
import argparse
import io
import cmd
import readline
import atexit
import sys

# Принудительно выставляем UTF-8 для консоли
if sys.stdout.encoding.lower() not in ('utf-8', 'utf8'):
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', line_buffering=True)
if sys.stderr.encoding.lower() not in ('utf-8', 'utf8'):
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding='utf-8', line_buffering=True)

from core.manager import EvolutionManager
from core.sdk import djb2_hash

class NeuroCoreShell(cmd.Cmd):
    intro = "\n" + "="*60 + "\n  NeuroCore Interactive Shell\n  Введите '?' или 'help' для списка команд.\n  ВНИМАНИЕ: Сначала введите 'start' для запуска C-ядра.\n" + "="*60 + "\n"
    prompt = '(neuro) > '

    def __init__(self, manager):
        super().__init__()
        self.manager = manager
        self.histfile = manager.root / "app" / ".neurocore_history"
        try:
            readline.read_history_file(self.histfile)
            readline.set_history_length(1000)
        except FileNotFoundError:
            pass
        atexit.register(readline.write_history_file, self.histfile)

    def do_start(self, arg):
        """start\nЗапустить C-ядро в фоне и подключиться к нему по IPC."""
        was_running = self.manager.is_core_responding()

        # Метод initialize() сам разберется: если ядро живое - подключится,
        # если мертвое - запустит и дождется ответа, а также проверит воркеры.
        self.manager.initialize()

        if was_running:
            print("[System] Ядро уже запущено. Подключение восстановлено.")
        else:
            print("[System] Ядро успешно запущено. Можно отправлять команды.")

    def do_stat(self, arg):
        """stat
        Show core statistics."""
        self.manager.execute_command("stat")

    def do_get_stats(self, arg):
        """get_stats
        Show core statistics."""
        self.manager.execute_command("get_stats")

    def do_retrieve(self, arg):
        """retrieve <keyword>
        Find facts in the knowledge graph by keyword."""
        if not arg: print("Usage: retrieve <keyword>")
        else: self.manager.execute_command("retrieve", arg)

    def do_learn(self, arg):
        """learn <text>
        Extract knowledge from text via LLM and store it in LMDB."""
        if not arg: print("Usage: learn <text>")
        else: self.manager.execute_command("learn", arg)

    def do_think(self, arg):
        """think
        Force a single MainLoop cycle in the C-core."""
        self.manager.execute_command("think")

    def do_bootstrap(self, arg):
        """bootstrap
        Initialize Meta-Core concepts and basic algorithms. Run once."""
        self.manager.execute_command("bootstrap")

    def do_test(self, arg):
        """test
        Run all AGI Olympic tests."""
        self.manager.run_tests()

    def do_ask(self, arg):
        """ask <query>
        Ask a question using the basic MVP Agent."""
        if not arg: print("Usage: ask <query>")
        else: self.manager.execute_command("ask", arg)

    def do_chat(self, arg):
        """chat <text>
        Talk to the conversational ChatService."""
        if not arg: print("Usage: chat <text>")
        else: self.manager.execute_command("chat", arg)

    def do_agent(self, arg):
        """agent <text>
        Talk to the Zero-Hardcode Mind Agent (synthesizes bytecode dynamically)."""
        if not arg: print("Usage: agent <text>")
        else: self.manager.execute_command("agent", arg)

    def do_ingest(self, arg):
        """ingest <file.txt>
        Parse a large text file in parallel and store in DB."""
        if not arg: print("Usage: ingest <file.txt>")
        else:
            import shlex
            # shlex.split правильно снимет кавычки с пути
            parsed_args = shlex.split(arg)
            self.manager.execute_command("ingest", *parsed_args)

    def do_shutdown(self, arg):
        """shutdown
        Gracefully stop the C-core and background workers."""
        print("[System] Остановка системы...")
        self.manager.execute_command("shutdown")
        return True

    def do_exit(self, arg):
        """exit
        Exit the shell."""
        print("Exiting...")
        return True

    def do_EOF(self, arg):
        """Выйти из оболочки (Ctrl+D)."""
        print()
        return True

    def do_recon(self, arg):
        """recon <domain.com>
        Запустить автоматический сбор поддоменов (SSL) и поиск уязвимостей."""
        if not arg:
            print("Usage: recon <domain.com>")
            return

        domain = arg.strip()
        goal_id = f"Recon_{djb2_hash(domain)}"

        # Создаем цель в LMDB и привязываем к ней домен
        self.manager.core_client.learn({"atoms": [{
            "id": goal_id,
            "process": "IS_A",
            "args": [goal_id, "ReconGoal"],
            "properties": {"target": domain}
        }]})

        # Активируем цель — планировщик C-ядра сам кинет её в очередь Recon Worker'а
        self.manager.core_client.activate_goal(goal_id, utility=0.9)
        self.manager.core_client.think()
        print(f"[*] Цель разведки создана для {domain}. Воркеры начали работу в фоне.")

    def default(self, line):
        """If command is not recognized, treat it as a chat message for MindAgent."""
        self.manager.execute_command("agent", line)


def main():
    parser = argparse.ArgumentParser(
        description="NeuroCore CLI and System Manager",
        formatter_class=argparse.RawDescriptionHelpFormatter
    )
    subparsers = parser.add_subparsers(dest="command", help="Commands")

    subparsers.add_parser("start", help="Start the C-core")
    subparsers.add_parser("think", help="Force a single MainLoop cycle")
    subparsers.add_parser("bootstrap", help="Initialize Meta-Core concepts")
    subparsers.add_parser("shutdown", help="Gracefully stop the C-core")
    subparsers.add_parser("test", help="Run AGI Olympic tests")
    subparsers.add_parser("stat", help="Show core statistics")
    subparsers.add_parser("get_stats", help="Show core statistics")

    parser_retrieve = subparsers.add_parser("retrieve")
    parser_retrieve.add_argument("query", nargs='+')

    parser_learn = subparsers.add_parser("learn")
    parser_learn.add_argument("text", nargs='+')

    parser_ask = subparsers.add_parser("ask")
    parser_ask.add_argument("query", nargs='+')

    parser_chat = subparsers.add_parser("chat")
    parser_chat.add_argument("text", nargs='+')

    parser_agent = subparsers.add_parser("agent")
    parser_agent.add_argument("text", nargs='+')

    parser_ingest = subparsers.add_parser("ingest")
    parser_ingest.add_argument("file")

    args = parser.parse_args()
    manager = EvolutionManager()

    try:
        if args.command is None:
            # Оболочка без автоматического старта ядра
            NeuroCoreShell(manager).cmdloop()
        else:
            manager.initialize()

            cmd_args = []
            if args.command == "retrieve": cmd_args = args.query
            elif args.command == "learn": cmd_args = args.text
            elif args.command == "ask": cmd_args = args.query
            elif args.command == "chat": cmd_args = args.text
            elif args.command == "agent": cmd_args = args.text
            elif args.command == "ingest": cmd_args = [args.file]

            if args.command == "test":
                manager.run_tests()
            elif args.command == "start":
                # Ядро уже было проинициализировано и запущено/подключено выше
                print("[System] Ядро запущено и готово к работе.")
            else:
                manager.execute_command(args.command, *cmd_args)

    except KeyboardInterrupt:
        print("\n[System] Interrupted by user.")
    except Exception as e:
        print(f"\n[FATAL] {e}", file=sys.stderr)
        sys.exit(1)
    finally:
        if args.command == "shutdown":
            manager.shutdown()
        else:
            # При exit просто отключаем IPC, не трогая само ядро
            try: manager.core_client.close()
            except Exception: pass

if __name__ == "__main__":
    main()
