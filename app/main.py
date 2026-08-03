#!/usr/bin/env python3
# app/main.py
import argparse
import sys
import io
import cmd

# Принудительно выставляем UTF-8 для консоли (решает проблему с 'ascii codec can't encode characters' в toolbx)
if sys.stdout.encoding.lower() not in ('utf-8', 'utf8'):
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', line_buffering=True)
if sys.stderr.encoding.lower() not in ('utf-8', 'utf8'):
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding='utf-8', line_buffering=True)

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
        self.histfile = manager.root / "app" / ".neurocore_history"
        try:
            readline.read_history_file(self.histfile)
            readline.set_history_length(1000)
        except FileNotFoundError:
            pass
        atexit.register(readline.write_history_file, self.histfile)

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
        """If command is not recognized, treat it as a chat message for MindAgent."""
        self.manager.execute_command("agent", line)

def main():
    parser = argparse.ArgumentParser(
        description="NeuroCore CLI and System Manager",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python main.py                # Start interactive shell
  python main.py agent "Hello"  # Send a message to the agent
  python main.py think          # Trigger the cognitive loop
  python main.py shutdown       # Stop the system
        """
    )
    subparsers = parser.add_subparsers(dest="command", help="Commands (leave empty for interactive shell)")

    subparsers.add_parser("think", help="Force a single MainLoop cycle")
    subparsers.add_parser("bootstrap", help="Initialize Meta-Core concepts")
    subparsers.add_parser("shutdown", help="Gracefully stop the C-core")

    parser_retrieve = subparsers.add_parser("retrieve", help="Find facts in the graph")
    parser_retrieve.add_argument("query", nargs='+', help="Keyword to search for")

    parser_learn = subparsers.add_parser("learn", help="Extract knowledge from text")
    parser_learn.add_argument("text", nargs='+', help="Text to learn from")

    parser_ask = subparsers.add_parser("ask", help="Ask a question using MVP Agent")
    parser_ask.add_argument("query", nargs='+', help="Question")

    parser_chat = subparsers.add_parser("chat", help="Talk to the conversational ChatService")
    parser_chat.add_argument("text", nargs='+', help="Message text")

    parser_agent = subparsers.add_parser("agent", help="Talk to the Zero-Hardcode Mind Agent")
    parser_agent.add_argument("text", nargs='+', help="Message text")

    parser_ingest = subparsers.add_parser("ingest", help="Parse a large text file")
    parser_ingest.add_argument("file", help="Path to the file")

    args = parser.parse_args()
    manager = EvolutionManager()

    try:
        if args.command is None:
            manager.initialize()
            NeuroCoreShell(manager).cmdloop()
        else:
            print(f"Executing: {args.command}")
            manager.initialize()

            cmd_args = []
            if args.command == "retrieve": cmd_args = args.query
            elif args.command == "learn": cmd_args = args.text
            elif args.command == "ask": cmd_args = args.query
            elif args.command == "chat": cmd_args = args.text
            elif args.command == "agent": cmd_args = args.text
            elif args.command == "ingest": cmd_args = [args.file]

            manager.execute_command(args.command, *cmd_args)

    except Exception as e:
        print(f"\n[FATAL] {e}", file=sys.stderr)
        sys.exit(1)
    finally:
        if args.command == "shutdown":
            manager.shutdown()

if __name__ == "__main__":
    main()
