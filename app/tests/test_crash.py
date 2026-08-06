# Crash Recovery:
# Скрипт test_crash.py должен запустить learn тяжелого пайплайна,
# сделать os.kill(core_pid, signal.SIGKILL), запустить ядро заново и через get_stats проверить,
# что LMDB не повреждена и кол-во атомов >= предыдущему дампу.
