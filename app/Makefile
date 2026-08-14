VENV := $(realpath ../.venv)

.PHONY: venv install run

venv:
	python3 -m venv ../.venv

install: venv
	. ../.venv/bin/activate && \
	pip install -U pip && \
	pip install -r requirements.txt

run:
	$(VENV)/bin/python main.py

clean:
	find . -name __pycache__ -type d -exec rm -rf {} +
	rm .neurocore_history
	rm llm_debug.log
	rm -r .pytest_cache
