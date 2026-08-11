# app/tests/conftest.py
import pytest
import sys
from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[1]
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

from core.manager import EvolutionManager

@pytest.fixture
def isolated_core(tmp_path):
    """
    Поднимает полностью изолированное C-ядро со своей чистой LMDB
    во временной директории, специфичной для конкретного теста.
    """
    test_db_dir = tmp_path / "lmdb_data"

    manager = EvolutionManager(db_path=str(test_db_dir))
    # Явно стартуем изолированное ядро для теста
    manager.start_core()
    manager.wait_core()

    try:
        yield manager.core_client
    finally:
        manager.shutdown()
