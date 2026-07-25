# Architecture

Version: 1.0

---

# Purpose

This document defines the high-level architecture of NeuroCore.

Unlike the documents in `docs/`, which describe individual subsystems, this document explains how the entire system fits together.

It serves as the architectural reference for all future development.

If implementation and this document disagree, the implementation is considered incorrect.

---

# Design Principles

NeuroCore is designed around several fundamental principles.

## 1. Separation of Responsibilities

Every module has exactly one responsibility.

A module must not perform work that belongs to another module.

Examples:

- Memory stores knowledge.
- Understanding creates knowledge.
- Reasoning derives knowledge.
- Planner builds plans.
- Execution performs actions.
- Learning improves the system.

---

## 2. Knowledge-Centric Architecture

Everything inside NeuroCore is represented as structured knowledge.

Not prompts.

Not documents.

Not raw text.

The internal representation is always a Knowledge Object.

---

## 3. Layered Architecture

Each layer depends only on lower layers.

```
Application

↓

Cognitive Layer

↓

Knowledge Layer

↓

Storage Layer

↓

Operating System
```

Dependencies must always point downward.

---

## 4. Loose Coupling

Subsystems communicate only through stable interfaces.

Never through direct implementation details.

Communication occurs through:

- Memory API
- IPC
- Plugin API
- Tool API

---

## 5. Explainability

Every output must be traceable.

The system must always be able to answer:

- Why was this conclusion produced?
- Which knowledge was used?
- Which reasoning strategy was selected?
- Which plan was executed?
- Which observations influenced learning?

Explainability is a core architectural requirement.

---

# System Overview

```
                 External World
                        │
                        ▼
                Understanding
                        │
                        ▼
                 Knowledge Model
                        │
                        ▼
                    Memory Engine
                        │
                 Activation Engine
                        │
                        ▼
                 Working Memory
                        │
                        ▼
                  Cognitive VM
                        │
     ┌──────────────────┼──────────────────┐
     ▼                  ▼                  ▼
 Reasoning          Planner          Execution
     │                  │                  │
     └──────────────────┼──────────────────┘
                        ▼
                   Observation
                        ▼
                     Episode
                        ▼
                     Learning
                        ▼
                     Memory
```

This loop represents the core of NeuroCore.

---

# Major Layers

## Layer 1 — Infrastructure

Responsible for technical foundations.

Includes:

- Runtime
- Configuration
- Logging
- Storage
- IPC
- Plugins

This layer knows nothing about cognition.

---

## Layer 2 — Knowledge

Responsible for representing information.

Includes:

- Knowledge Objects
- Relations
- Concepts
- Rules
- Algorithms
- Episodes
- Observations

This is the language spoken by every cognitive subsystem.

---

## Layer 3 — Memory

Responsible for storing and retrieving knowledge.

Includes:

- Long-Term Memory
- Working Memory
- Activation
- Search
- Statistics
- Versioning

---

## Layer 4 — Cognition

Responsible for thinking.

Includes:

- Understanding
- Reasoning
- Planner
- Execution
- Learning

This layer transforms knowledge into actions and new knowledge.

---

## Layer 5 — External Interaction

Responsible for interacting with the outside world.

Includes:

- Tools
- Plugins
- APIs
- Files
- Databases
- Browsers
- LLMs
- Operating System

---

# Cognitive Pipeline

Every task follows the same high-level pipeline.

```
Input

↓

Understanding

↓

Knowledge

↓

Activation

↓

Reasoning

↓

Planning

↓

Execution

↓

Observation

↓

Episode

↓

Learning

↓

Updated Memory
```

This cycle is the foundation of continuous improvement.

---

# Core Data Flow

NeuroCore is fundamentally a data transformation system.

```
Raw Information

↓

Structured Knowledge

↓

Activated Knowledge

↓

Reasoned Knowledge

↓

Plan

↓

Action

↓

Experience

↓

Improved Knowledge
```

Each stage has exactly one responsibility.

---

# Memory Flow

```
Long-Term Memory

↓

Activation

↓

Working Memory

↓

Reasoning

↓

New Knowledge

↓

Memory
```

Knowledge is never modified directly during reasoning.

New knowledge is produced and then stored.

---

# Execution Flow

```
Goal

↓

Planner

↓

Plan

↓

Execution

↓

Tool

↓

Observation

↓

Episode
```

Execution never changes memory directly.

Learning decides what should be retained.

---

# Learning Flow

```
Episode

↓

Analysis

↓

Pattern Discovery

↓

Confidence Update

↓

Relation Update

↓

Memory Optimization
```

Learning operates only after execution.

---

# Runtime Flow

```
Runtime

↓

Component Manager

↓

Subsystem

↓

Health Monitor

↓

Recovery
```

Runtime manages components but does not perform cognition.

---

# Dependency Rules

Allowed:

```
Reasoning

↓

Memory
```

Forbidden:

```
Memory

↓

Reasoning
```

Allowed:

```
Execution

↓

Tool API
```

Forbidden:

```
Tool

↓

Execution
```

Allowed:

```
Plugins

↓

Plugin API
```

Forbidden:

```
Plugin

↓

Internal Memory Structures
```

Dependencies must always move toward abstraction.

---

# Communication Rules

Subsystems never call each other directly.

Instead:

```
Component

↓

API

↓

Component
```

or

```
Component

↓

IPC

↓

Component
```

This keeps the architecture replaceable.

---

# Error Propagation

Every error follows the same lifecycle.

```
Failure

↓

Error Object

↓

IPC Event

↓

Logger

↓

Episode

↓

Learning
```

Errors are treated as knowledge.

---

# Extensibility

Everything that depends on external technology is isolated.

Examples:

- Python
- Git
- Docker
- OCR
- Browser
- LLM

These are replaceable implementations behind stable interfaces.

---

# Architectural Invariants

The following rules must never be violated.

- Memory owns knowledge.
- Understanding creates knowledge.
- Reasoning derives knowledge.
- Planner creates plans.
- Execution performs actions.
- Learning modifies memory.
- Runtime manages lifecycle.
- IPC manages communication.
- VM manages active cognition.
- Tools access the outside world.
- Plugins extend functionality.
- Любая write-транзакция LMDB создаётся только внутри db_writer потока. Все остальные места (subconscious.c, IPC-хендлеры, VM-worker'ы) либо (а) читают в MDB_RDONLY-транзакции, открытой в своём же потоке, либо (б) формируют DbWriteFn и кладут в очередь через db_write_sync/db_write_async.

If a feature breaks these invariants, the architecture must be redesigned rather than patched.

---

# Scalability

The architecture is designed to scale in three dimensions.

## Functional

New capabilities can be added through plugins.

## Performance

Components can become multithreaded without changing interfaces.

## Distributed

Components can eventually run on different machines while preserving the same architecture.

---

# Architectural Goal

The long-term objective is to create a reusable cognitive architecture rather than a single application.

Every implementation decision should preserve:

- modularity;
- explainability;
- maintainability;
- extensibility;
- long-term evolution.

---

# Final Principle

The architecture is the product.

The implementation is only one possible realization of that architecture.

If implementation becomes easier by violating architecture, the implementation is wrong—not the architecture.
