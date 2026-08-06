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
