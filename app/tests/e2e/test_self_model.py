# app/tests/e2e/test_self_model.py
#
import pytest

from core.sdk import CoreClient
from core.bootstrap import bootstrap_knowledge


EXPECTED_TYPES = {
    "Entity",
    "Relation",
    "Process",
    "ExecutableKnowledge",
    "Algorithm",
    "Skill",
    "Capability",
    "Policy",
    "SelectionPolicy",
    "PlanningPolicy",
    "Goal",
    "State",
    "Observation",
    "Action",
    "Evidence",
    "Belief",
    "Prediction",
    "Error",
    "Environment",
    "Implementation",
    "Affordance",
    "Profile",
    "CognitiveSystem",
    "KOSMOS",
    "Component",
    "CognitiveVM",
    "KnowledgeStore",
    "Invocation",
    "Episode",
}


EXPECTED_RELATIONS = {
    "IMPLEMENTS",
    "PROVIDES",
    "REQUIRES",
    "USES",
    "SELECTS",
    "PRODUCES",
    "PREDICTS",
    "VERIFIED_BY",
    "HAS_BELIEF",
    "SUPPORTED_BY",
    "FAILS_WITH",
    "HAS_AFFORDANCE",
    "HAS_COMPONENT",
    "TARGETS",
    "PART_OF",
    "RESULTS_IN",
}


@pytest.fixture
def core(isolated_core):
    # Используем изолированное ядро из conftest.py,
    # чтобы боевая база (со своими RAG-фактами) не ломала проверки
    bootstrap_knowledge(isolated_core._ipc, force=True)
    return isolated_core


def atom_matches(atom, process, args):
    return (
        atom.get("process") == process
        and [str(x) for x in atom.get("args", [])]
        == [str(x) for x in args]
    )


def get_atoms(core, query):
    response = core.retrieve(query)

    if not isinstance(response, dict):
        return []

    atoms = response.get("atoms", [])

    return atoms if isinstance(atoms, list) else []


def find_relation(core, process, args):
    atoms = get_atoms(core, args[0])

    return any(
        atom_matches(atom, process, args)
        for atom in atoms
    )


def test_self_model_contains_kosmos(core):
    atoms = get_atoms(core, "KOSMOS")

    assert any(
        atom.get("process") == "IS_A"
        and atom.get("args") == ["KOSMOS", "CognitiveSystem"]
        for atom in atoms
    )


def test_self_model_contains_vm(core):
    atoms = get_atoms(core, "CognitiveVM")

    assert any(
        atom.get("process") == "IS_A"
        and atom.get("args") == ["CognitiveVM", "Component"]
        for atom in atoms
    )


def test_kosmos_has_vm_component(core):
    assert find_relation(
        core,
        "HAS_COMPONENT",
        ["KOSMOS", "CognitiveVM"],
    )


def test_kosmos_has_knowledge_store(core):
    assert find_relation(
        core,
        "HAS_COMPONENT",
        ["KOSMOS", "KnowledgeStore"],
    )


@pytest.mark.parametrize("entity_type", sorted(EXPECTED_TYPES))
def test_bootstrap_contains_entity_type(core, entity_type):
    atoms = get_atoms(core, entity_type)

    assert any(
        atom.get("process") == "IS_A"
        and len(atom.get("args", [])) >= 2
        and atom["args"][0] == entity_type
        for atom in atoms
    ), f"Missing ontology entity: {entity_type}"


@pytest.mark.parametrize("relation", sorted(EXPECTED_RELATIONS))
def test_bootstrap_contains_relation_type(core, relation):
    atoms = get_atoms(core, relation)

    assert any(
        atom.get("process") == "IS_A"
        and len(atom.get("args", [])) >= 2
        and atom["args"][0] == relation
        and atom["args"][1] == "Relation"
        for atom in atoms
    ), f"Missing relation type: {relation}"
