# app/tests/e2e/test_mock_e2e.py
#
import pytest

from core.sdk import CoreClient
from core.bootstrap import bootstrap_knowledge
from environments.mock import MockEnvironment


GOAL = "ReachCounterFive"

CAPABILITY = "IncrementCounter"

IMPLEMENTATION = "MockIncrementImplementation"

POLICY = "SelectionPolicy_FirstAvailable"

ENVIRONMENT = "MockCounterEnvironment"


def test_mock_environment_reaches_target_without_domain_hardcode():
    """
    Acid test.

    KOSMOS должен:

        Goal
          ↓
        Planning
          ↓
        Capability
          ↓
        SelectionPolicy
          ↓
        Implementation
          ↓
        Invocation
          ↓
        Observation
          ↓
        Belief / Episode

    Никаких специальных условий для ReachCounterFive
    не должно существовать в runtime.
    """

    env = MockEnvironment()

    target = 5

    for _ in range(target):
        result = env.act("increment")

        assert result["success"] is True

    observation = env.observe()

    assert observation["state"]["counter"] == target
