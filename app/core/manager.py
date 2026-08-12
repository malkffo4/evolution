# app/core/manager.py
import signal, subprocess, time, os, sys, json
from pathlib import Path

from core.ipc import DEFAULT_SOCKET, LOCK_FILE
from core.sdk import CoreClient
from core.llm import LLMClient
from core.bootstrap import bootstrap_knowledge

from services.chat_service import ChatService
from services.mvp_agent import MvpAgent
from services.mind_agent import MindAgent

BUILD_MODE = "debug"

class EvolutionManager:
    def __init__(self, db_path=None):
        self.root = Path(__file__).resolve().parents[2]
        self.core_dir = self.root / "core"
        self.core_bin = self.core_dir / "build" / BUILD_MODE / "bin" / "evolution_core"
        self.makefile = self.core_dir / "Makefile"

        self.core_process = None
        self.db_path = db_path  # Путь для LMDB (если передан)

        self.core_client = CoreClient(timeout=2.0)
        self.llm_client = LLMClient()
        self.ipc = self.core_client._ipc

        self.running = False

        self.research_worker = self.root / "app" / "services" / "research_worker.py"
        self.research_worker_process = None

        self.chat = ChatService(self.ipc, self.llm_client)
        self.mvp = MvpAgent(self.ipc, self.llm_client)
        self.mind = MindAgent(self.core_client, self.llm_client)

        self.core_log_path = "/tmp/evolution_core.log"
        self.core_log_fd = None
        self.research_worker_log_path = "/tmp/evolution_research_worker.log"
        self.research_worker_log_fd = None

    def initialize(self):
        self.check_project()
        if self.is_core_responding():
            self.connect_ipc()
        else:
            self.start_core()
            self.wait_core()
        try:
            self.start_research_worker()
        except Exception:
            self.shutdown()
            raise

    def check_project(self):
        if not self.makefile.exists(): raise RuntimeError("core/Makefile not found.")
        if not self.core_dir.exists(): raise RuntimeError("core directory not found.")

    def is_core_responding(self):
        if not os.path.exists(DEFAULT_SOCKET): return False
        try:
            test = CoreClient(timeout=2.0)
            test.connect()
            test.close()
            return True
        except Exception:
            return False

    def wait_core(self, attempts=30, interval=0.5):
        for _ in range(attempts):
            if self.core_process and self.core_process.poll() is not None:
                raise RuntimeError(f"Core died! Check {self.core_log_path}")
            try:
                self.connect_ipc()
                return
            except Exception:
                time.sleep(interval)
        raise RuntimeError("IPC timeout. Core might be stuck.")

    def start_core(self):
        self._cleanup_orphans()

        # УБИРАЕМ ЗОМБИ: заставляем Python прочитать статус
        if self.core_process:
            try: self.core_process.wait(timeout=0.1)
            except Exception: pass

        for stale_file in [DEFAULT_SOCKET, LOCK_FILE]:
            if os.path.exists(stale_file):
                try: os.unlink(stale_file)
                except OSError: pass

        cmd = [str(self.core_bin)]
        if self.db_path:
            cmd.extend(["--db-path", str(self.db_path)])

        self.core_log_fd = open(self.core_log_path, "w+")
        self.core_process = subprocess.Popen(
            cmd,
            cwd=self.core_dir,
            stdout=self.core_log_fd,
            stderr=subprocess.STDOUT,
        )
        time.sleep(1.5)

    def start_research_worker(self):
        try:
            if self.research_worker_process and self.research_worker_process.poll() is None:
                return
            if os.path.exists(self.research_worker_log_path):
                try: os.unlink(self.research_worker_log_path)
                except OSError: pass
            self.research_worker_log_fd = open(self.research_worker_log_path, "w+")
            self.research_worker_process = subprocess.Popen(
                [sys.executable, "-u", str(self.research_worker)],
                cwd=self.root,
                stdout=self.research_worker_log_fd,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
            time.sleep(0.3)
        except Exception as e:
            self._stop_research_worker(force=True)
            raise RuntimeError(f"Failed to start ResearchWorker: {e}") from e

    def _stop_research_worker(self, force=False):
        proc = self.research_worker_process
        if not proc: return
        try:
            if proc.poll() is None:
                try:
                    proc.terminate()
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    if force:
                        proc.kill()
                        proc.wait(timeout=3)
        finally:
            if self.research_worker_log_fd:
                try: self.research_worker_log_fd.close()
                except Exception: pass
                self.research_worker_log_fd = None

    def connect_ipc(self):
        self.core_client.connect()

    def run_tests(self):
        print("\n[Manager] Запускаем полный набор тестов (E2E + Olympics)...")
        script_path = self.root / "app" / "tests" / "run_all.py"
        try:
            subprocess.run([sys.executable, str(script_path)], check=True)
        except subprocess.CalledProcessError as e:
            print(f"[Manager] Тесты завершились с ошибкой: {e}", file=sys.stderr)

    def format_and_print_response(self, response):
        if not response:
            print("[System] No response received.")
            return
        payload_raw = response.get("payload", "")

        if isinstance(payload_raw, dict):
            if "reply" in payload_raw: print(f"\nAI: {payload_raw['reply']}"); return
            if payload_raw.get("ok") is True: print(f"\n[OK] {payload_raw.get('msg', 'Command completed successfully.')}"); return
            if "error" in payload_raw: print(f"\n[ERROR] {payload_raw['error']}"); return

        if isinstance(payload_raw, str) and payload_raw.strip():
            try:
                payload = json.loads(payload_raw)
                if isinstance(payload, dict):
                    if "reply" in payload: print(f"\nAI: {payload['reply']}"); return
                    if payload.get("ok") is True: print(f"\n[OK] {payload.get('msg', 'Command completed successfully.')}"); return
                    if "error" in payload: print(f"\n[ERROR] {payload['error']}"); return
            except json.JSONDecodeError:
                pass
            print(f"\nAI: {payload_raw}")
        elif not isinstance(payload_raw, dict):
            print(f"\nAI (Raw): {response}")
        else:
            print(response)

    def execute_command(self, cmd_name: str, *args):
        cmd_name = cmd_name.lower()
        if cmd_name == "shutdown":
            self.shutdown()
            return False

        elif cmd_name == "retrieve":
            keyword = " ".join(args)
            try:
                from knowledge.retrieval import retrieve
                res_text = retrieve(self.ipc, keyword)
                print(f"\n{res_text}" if res_text else "\n[Retrieval] Фактов не найдено.")
            except Exception as e: print(f"[ERROR] {e}")

        elif cmd_name == "learn":
            text_to_learn = " ".join(args)
            print("[Learner] Extracting triplets and writing to C-core...")
            try:
                graph = self.mvp.extract_atoms(text_to_learn)
                resp = self.mvp.store_atoms(graph)
                print(f"[Learner] atoms={len(graph.get('atoms', []))} -> {resp.get('payload')}")
            except Exception as e: print(f"[ERROR] {e}")

        elif cmd_name == "think":
            try:
                self.core_client.think()
                print("\n[OK] MainLoop triggered.")
            except Exception as e: print(f"[ERROR] {e}")

        elif cmd_name == "bootstrap":
            bootstrap_knowledge(self.ipc, force=True)
            print("[Manager] Bootstrap complete.")

        elif cmd_name == "ask":
            query = " ".join(args)
            try:
                reply = self.mvp.step(query)
                print(f"\nAI (MVP): {reply}")
            except Exception as e: print(f"[ERROR] {e}")

        elif cmd_name == "chat":
            text = " ".join(args)
            try:
                reply = self.chat.answer(text)
                print(f"\nAI: {reply}")
            except Exception as e: print(f"[ERROR] {e}")

        elif cmd_name == "agent":
            text = " ".join(args)
            try:
                reply = self.mind.think(text)
                print(f"\nAI: {reply}")
            except Exception as e: print(f"[ERROR] {e}")

        elif cmd_name in ("get_stats", "stat"):
            try:
                stats = self.core_client.get_stats()
                print(f"\n[Stats]:\n{json.dumps(stats, indent=2)}")
            except Exception as e:
                print(f"[ERROR] {e}")

        elif cmd_name == "ingest":
            file_path = args[0] if args else None
            if not file_path:
                print("Usage: ingest <file_path> [--provider ollama]")
            else:
                from tools.ingest_knowledge import main as run_ingest
                sys.argv = ["ingest_knowledge.py", args[0]] + list(args[1:])
                run_ingest()
        else:
            print(f"Unknown command: {cmd_name}")
        return True

    def shutdown(self):
        if getattr(self, "_shutdown_done", False): return
        self._shutdown_done = True
        self.running = False

        if self.is_core_responding():
            try:
                self.core_client._command("shutdown")
            except Exception:
                pass

        try: self.core_client.close()
        except Exception: pass
        self._stop_research_worker(force=True)

        # Даем ядру 2 секунды на сохранение после отправки IPC команды "shutdown"
        if self.core_process:
            try:
                self.core_process.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                # Если не вышло само - шлем мягкий сигнал (SIGTERM)
                self.core_process.terminate()
                try:
                    self.core_process.wait(timeout=1.0)
                except subprocess.TimeoutExpired:
                    # Если наглухо висит - убиваем жестко (SIGKILL)
                    self.core_process.kill()
                    self.core_process.wait(timeout=1.0)
            self.core_process = None
        else:
            # Если мы не владеем процессом, но он висит в ОС
            self._cleanup_orphans()

        # УБИРАЕМ ЗОМБИ при завершении
        if self.core_process:
            try: self.core_process.wait(timeout=1)
            except Exception: pass

        if os.path.exists(DEFAULT_SOCKET):
            try: os.unlink(DEFAULT_SOCKET)
            except Exception: pass

        if getattr(self, 'core_log_fd', None):
            try: self.core_log_fd.close()
            except: pass

    def _cleanup_orphans(self):
        """Интеллектуальная зачистка: убиваем только если есть кого, не тратя время впустую."""
        # Проверяем, есть ли вообще такой процесс (returncode == 0 значит найден)
        if subprocess.run(["pgrep", "-f", self.core_bin.name], stdout=subprocess.DEVNULL).returncode != 0:
            return # Никого нет, уходим моментально!

        # Процесс есть. Посылаем SIGTERM (мягко)
        subprocess.run(["pkill", "-15", "-f", self.core_bin.name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        # Ждем максимум 2 секунды, проверяя статус каждые 0.1с
        for _ in range(20):
            if subprocess.run(["pgrep", "-f", self.core_bin.name], stdout=subprocess.DEVNULL).returncode != 0:
                return # Умер сам, отлично
            time.sleep(0.1)

        # Если за 2 секунды не сдался - добиваем SIGKILL
        subprocess.run(["pkill", "-9", "-f", self.core_bin.name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
