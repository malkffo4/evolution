# NeuroCore

> **A modular cognitive architecture written in C.**
>
> NeuroCore is not another AI agent.
> It is an attempt to build a general cognitive core capable of storing knowledge, understanding information, reasoning, planning, learning, and interacting with the outside world through a clean, extensible architecture.

---

# Vision

Modern LLMs are extremely good at predicting text.

Humans do much more.

Humans:

- build internal models of the world;
- accumulate long-term knowledge;
- reason using multiple strategies;
- plan before acting;
- remember experiences;
- improve from mistakes;
- explain decisions;
- reuse previously acquired skills.

NeuroCore is an attempt to build this missing layer.

Instead of creating another chatbot, the goal is to build a **general-purpose cognitive engine**.

---

# Philosophy

NeuroCore follows several fundamental principles.

## Knowledge First

Everything inside the system is represented as knowledge.

Not text.

Not prompts.

Not embeddings.

Knowledge.

---

## Modular Architecture

Every subsystem has exactly one responsibility.

No module should perform work belonging to another module.

---

## Explainability

Every decision must be explainable.

Every conclusion must have its origin.

Every hypothesis must indicate why it exists.

---

## Long-Term Evolution

The architecture is designed to evolve for many years.

The goal is not to build a prototype.

The goal is to build a cognitive platform.

---

## Technology Independence

The architecture is independent of:

- LLM provider;
- database implementation;
- operating system;
- programming language bindings;
- external tools.

Everything is replaceable.

---

# Architecture Overview

```
                External World
                       │
                       ▼
               Understanding
                       │
                       ▼
                  Knowledge
                       │
                       ▼
                   Memory
                       │
                Activation Engine
                       │
                       ▼
                Working Memory
                       │
                       ▼
                       VM
                       │
        ┌──────────────┼──────────────┐
        │              │              │
        ▼              ▼              ▼
   Reasoning      Planner      Execution
        │              │              │
        └──────────────┼──────────────┘
                       │
                       ▼
                   Learning
                       │
                       ▼
                    Memory
```

This cycle never stops.

Every interaction improves the system.

---

# Core Components

## Memory

Stores all knowledge.

Responsible for:

- knowledge storage;
- indexing;
- activation;
- statistics;
- history;
- retrieval.

---

## Understanding

Transforms external information into internal knowledge.

Supports:

- books;
- documentation;
- source code;
- images;
- speech;
- APIs;
- structured data.

---

## Reasoning

Produces new knowledge from existing knowledge.

Implements:

- deduction;
- induction;
- abduction;
- analogy;
- causal reasoning;
- constraint reasoning.

---

## Planner

Transforms goals into executable plans.

Responsible for:

- decomposition;
- optimization;
- risk analysis;
- algorithm selection.

---

## Execution

Executes plans.

Responsible for:

- tools;
- runtime state;
- observations;
- episodes;
- execution results.

---

## Learning

Improves the entire system using experience.

Updates:

- confidence;
- relation weights;
- statistics;
- activation;
- algorithm ratings.

---

## VM

The cognitive runtime.

Coordinates:

- working memory;
- context;
- goals;
- attention;
- tasks;
- thought execution.

---

## Runtime

Controls the lifecycle of every subsystem.

Responsible for:

- initialization;
- monitoring;
- shutdown;
- recovery;
- configuration.

---

## IPC

Provides communication between all components.

No component communicates directly with another.

---

## Plugins

Allows extending NeuroCore without changing the core.

Everything external is implemented as a plugin.

---

## Tools

Provide access to the outside world.

Examples:

- Python;
- Git;
- Browser;
- Shell;
- HTTP;
- OCR;
- LLM.

---

# Project Goals

NeuroCore aims to become a system capable of:

- understanding arbitrary information;
- building structured knowledge;
- reasoning;
- planning;
- learning continuously;
- explaining its conclusions;
- interacting with external tools;
- improving over time.

---

# Non-Goals

NeuroCore is **not** intended to be:

- another LLM wrapper;
- a prompt engineering framework;
- an autonomous agent built entirely around prompts;
- a chatbot;
- an embedding database;
- a vector search engine.

Those technologies may be used by NeuroCore.

They are not NeuroCore itself.

---

# Technology Stack

Current implementation targets:

- Language: C23
- Build System: CMake
- Storage: LMDB
- Serialization: FlatBuffers / MessagePack (TBD)
- Testing: CTest
- Documentation: Markdown
- Operating System: Linux (primary)

Future support:

- Windows
- macOS
- BSD

---

# Development Principles

Architecture first.

Implementation second.

Optimization third.

No feature is accepted if it violates architecture.

---

# Project Status

Current stage:

```
Architecture Design
████████████████████ 100%

Implementation
░░░░░░░░░░░░░░░░░░░░   0%
```

The architecture is intentionally designed before implementation.

This minimizes future rewrites.

---

# Documentation

Project documentation is organized as follows:

```
docs/

01_Architecture.md
02_Knowledge.md
03_Memory.md
04_Understanding.md
05_Reasoning.md
06_Planner.md
07_Execution.md
08_Learning.md
09_VM.md
10_Runtime.md
11_IPC.md
12_Plugins.md
13_Tools.md
14_LearningLoop.md
15_Roadmap.md
```

Each document describes one subsystem.

Together they define the complete architecture.

---

# Long-Term Vision

The long-term objective is not merely to create software.

The objective is to build a reusable cognitive architecture that can serve as the foundation for future intelligent systems.

Every design decision is evaluated against one question:

> "Will this still make sense ten years from now?"

If the answer is no, the design is reconsidered.

---

# License

License has not yet been selected.

---

# Current Priority

Current focus:

1. Finalize architecture.
2. Review documentation.
3. Build the project foundation.
4. Implement the storage layer.
5. Implement the knowledge model.
6. Implement the memory subsystem.
7. Continue according to the roadmap.

---

# Final Statement

NeuroCore is an engineering project focused on building a long-lived cognitive architecture rather than a short-lived AI application.

The objective is to create a modular, explainable, extensible and continuously learning system whose capabilities emerge from the interaction of well-defined components instead of being concentrated inside a single monolithic model.
