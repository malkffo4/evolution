# app/tests/e2e/test_mock_environment.py
#
import pytest

from environments.base import EnvironmentRuntime
from environments.mock import MockEnvironment


def test_mock_environment_implements_contract():
    env = MockEnvironment()

    assert isinstance(env, EnvironmentRuntime)

    observation = env.observe()

    assert isinstance(observation, dict)
    assert "state" in observation
    assert "affordances" in observation

    assert observation["state"]["counter"] == 0

    assert set(observation["affordances"]) == {
        "increment",
        "decrement",
        "reset",
    }


def test_increment_changes_state():
    env = MockEnvironment()

    result = env.act("increment")

    assert result["success"] is True
    assert result["observation"]["counter"] == 1

    current = env.observe()

    assert current["state"]["counter"] == 1


def test_multiple_increment_actions():
    env = MockEnvironment()

    for _ in range(5):
        result = env.act("increment")
        assert result["success"] is True

    assert env.observe()["state"]["counter"] == 5


def test_decrement_changes_state():
    env = MockEnvironment()

    env.act("increment")
    env.act("increment")

    result = env.act("decrement")

    assert result["success"] is True
    assert result["observation"]["counter"] == 1
    assert env.observe()["state"]["counter"] == 1


def test_reset_restores_initial_state():
    env = MockEnvironment()

    env.act("increment")
    env.act("increment")
    env.act("increment")

    assert env.observe()["state"]["counter"] == 3

    result = env.act("reset")

    assert result["success"] is True
    assert result["observation"]["counter"] == 0
    assert env.observe()["state"]["counter"] == 0


def test_unknown_affordance_does_not_mutate_state():
    env = MockEnvironment()

    before = env.observe()["state"]["counter"]

    result = env.act("does_not_exist")

    assert result["success"] is False
    assert "error" in result

    after = env.observe()["state"]["counter"]

    assert after == before


@pytest.mark.parametrize(
    "affordance, expected",
    [
        ("increment", 1),
        ("decrement", -1),
        ("reset", 0),
    ],
)
def test_all_declared_affordances_are_executable(affordance, expected):
    env = MockEnvironment()

    if affordance == "reset":
        env.act("increment")
        env.act("increment")

    result = env.act(affordance)

    assert result["success"] is True
    assert result["observation"]["counter"] == expected
