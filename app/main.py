#!/usr/bin/env python3
# app/main.py

import argparse
import sys
import cmd
import readline
import atexit
from pathlib import Path

from core.manager import EvolutionManager

class NeuroCoreShell(cmd.Cmd):
    intro = "\n" + "="*60 + "\n  NeuroCore Interactive Shell\n  Type 'help' or '?' to list commands.\n" + "="*60 + "\n"
    prompt = '(neuro) > '

    def __init__(self, manager):
        super().__init__()
        self.manager = manager

        # Настройка истории
        self.histfile = manager.root / "app" / ".neurocore_history"
        try:
            readline.read_history_file(self.histfile)
            readline.set_history_length(1000)
        except FileNotFoundError:
            pass
        atexit.register(readline.write_history_file, self.histfile)

    # --- Команды оболочки ---

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

    def do_ask(self, arg):
        """ask <query>
        Ask a question using the basic MVP Agent."""
        if not arg: print("Usage: ask <query>")
        else: self.manager.execute_command("ask", arg)

    def do_chat(self, arg):
        """chat <text>
        Talk to the advanced ChatService with Semantic Compiler."""
        if not arg: print("Usage: chat <text>")
        else: self.manager.execute_command("chat", arg)

    def do_ingest(self, arg):
        """ingest <file.txt>
        Parse a large text file in parallel and store in DB."""
        if not arg: print("Usage: ingest <file.txt>")
        else: self.manager.execute_command("ingest", arg)

    def do_shutdown(self, arg):
        """shutdown
        Gracefully stop the C-core and background workers."""
        self.manager.execute_command("shutdown")
        return True # Exit shell

    def do_exit(self, arg):
        """exit
        Exit the shell. The core keeps running in the background."""
        print("Leaving shell. Core remains running.")
        return True

    def do_EOF(self, arg):
        print()
        return True

    def default(self, line):
        """If command is not recognized, treat it as a chat message."""
        self.manager.execute_command("chat", line)


def main():
    parser = argparse.ArgumentParser(
        description="NeuroCore CLI and System Manager",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python main.py                # Start interactive shell
  python main.py chat "Hello!"  # Send a message to the agent
  python main.py think          # Trigger the cognitive loop
  python main.py shutdown       # Stop the system
        """
    )

    # Subparsers for direct CLI execution
    subparsers = parser.add_subparsers(dest="command", help="Commands (leave empty for interactive shell)")

    # Define subcommands matching the shell
    subparsers.add_parser("think", help="Force a single MainLoop cycle")
    subparsers.add_parser("bootstrap", help="Initialize Meta-Core concepts")
    subparsers.add_parser("shutdown", help="Gracefully stop the C-core")

    parser_retrieve = subparsers.add_parser("retrieve", help="Find facts in the graph")
    parser_retrieve.add_argument("query", nargs='+', help="Keyword to search for")

    parser_learn = subparsers.add_parser("learn", help="Extract knowledge from text")
    parser_learn.add_argument("text", nargs='+', help="Text to learn from")

    parser_ask = subparsers.add_parser("ask", help="Ask a question using MVP Agent")
    parser_ask.add_argument("query", nargs='+', help="Question")

    parser_chat = subparsers.add_parser("chat", help="Talk to the advanced ChatService")
    parser_chat.add_argument("text", nargs='+', help="Message text")

    parser_ingest = subparsers.add_parser("ingest", help="Parse a large text file")
    parser_ingest.add_argument("file", help="Path to the file")

    args = parser.parse_args()

    manager = EvolutionManager()

    try:
        if args.command is None:
            # Интерактивный режим (REPL)
            manager.initialize()
            NeuroCoreShell(manager).cmdloop()
        else:
            # Разовое выполнение команды из CLI
            print(f"Executing: {args.command}")
            manager.initialize()

            # Извлекаем аргументы команды, если они есть
            cmd_args = []
            if args.command == "retrieve": cmd_args = args.query
            elif args.command == "learn": cmd_args = args.text
            elif args.command == "ask": cmd_args = args.query
            elif args.command == "chat": cmd_args = args.text
            elif args.command == "ingest": cmd_args = [args.file]

            manager.execute_command(args.command, *cmd_args)

            # Если мы только запустили ядро (и оно не работало в фоне),
            # возможно имеет смысл дать ему время или погасить.
            # Для CLI утилит обычно принято гасить после завершения разовой задачи,
            # но так как ядро имеет состояние, оставим его работать, как сервер базы данных.

    except Exception as e:
        print(f"\n[FATAL] {e}", file=sys.stderr)
        sys.exit(1)
    finally:
        # Если была запрошена команда shutdown, или мы упали,
        # или если мы сами подняли ядро чисто для одной команды — глушим.
        if args.command == "shutdown":
            manager.shutdown()
        # В REPL мы гасимся только если был вызван shutdown,
        # иначе оставляем ядро жить в фоне.

if __name__ == "__main__":
    main()
