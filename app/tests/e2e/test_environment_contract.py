# app/tests/e2e/test_environment_contract.py
#
import pytest

from environments.mock import MockEnvironment


@pytest.fixture
def environment():
    return MockEnvironment()


def test_observe_is_deterministic_without_action(environment):
    first = environment.observe()
    second = environment.observe()

    assert first == second


def test_observation_contains_state(environment):
    observation = environment.observe()

    assert isinstance(observation["state"], dict)


def test_observation_contains_affordances(environment):
    observation = environment.observe()

    affordances = observation["affordances"]

    assert isinstance(affordances, list)
    assert len(affordances) > 0


def test_action_returns_structured_result(environment):
    result = environment.act("increment")

    assert isinstance(result, dict)
    assert "success" in result
    assert isinstance(result["success"], bool)


def test_successful_action_produces_observation(environment):
    result = environment.act("increment")

    assert result["success"] is True
    assert "observation" in result
    assert isinstance(result["observation"], dict)


def test_failed_action_is_explicit(environment):
    result = environment.act("__invalid__")

    assert result["success"] is False
    assert "error" in result
