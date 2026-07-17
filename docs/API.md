# API Design Guidelines

Version: 1.0

---

# Purpose

This document defines the API philosophy and engineering standards used throughout NeuroCore.

The API is one of the most important parts of the project.

Good architecture can be destroyed by a poor API.

Every public function, interface, module and subsystem must follow the rules defined here.

---

# Philosophy

The API must be:

- simple;
- predictable;
- explicit;
- deterministic;
- composable;
- testable;
- thread-safe whenever possible.

The API exists for developers.

Not for implementations.

---

# Core Principles

## 1. Explicit is Better Than Implicit

Every function should clearly express:

- what it needs;
- what it changes;
- what it returns;
- who owns memory.

Never hide side effects.

Bad

```c
update(node);
```

Good

```c
knowledge_update_relation(node, relation);
```

---

## 2. Single Responsibility

One function.

One purpose.

Bad

```c
memory_load_and_parse_file();
```

Good

```c
memory_load();

understanding_parse();

knowledge_store();
```

---

## 3. Small Interfaces

Interfaces should expose only what is necessary.

Avoid "god objects".

Avoid huge headers.

---

## 4. Stable Contracts

Public APIs must remain stable.

Implementation may change.

Behavior must not.

---

## 5. Hide Implementation

Headers expose interfaces.

Source files contain implementation.

Never expose internal structures unless absolutely necessary.

Bad

```c
struct Memory {
    ...
};
```

Good

```c
typedef struct Memory Memory;
```

---

# Module API

Every module exposes only one public header.

Example

```
memory/

memory.h

memory.c

memory_internal.h
```

Applications include only

```
memory.h
```

---

# Public vs Internal

Public

```
include/

memory.h
planner.h
reasoning.h
runtime.h
```

Internal

```
src/

memory_internal.h

planner_internal.h
```

Internal headers must never be included outside their module.

---

# Naming

Public API uses snake_case.

Examples

```c
memory_create()

memory_destroy()

memory_insert()

planner_create()

planner_run()

reasoning_execute()
```

---

# Prefixes

Every module owns its namespace.

Examples

```
memory_

knowledge_

runtime_

planner_

reasoning_

execution_

learning_

ipc_

vm_

plugin_

tool_
```

Never create generic names.

Bad

```c
create();

run();

execute();
```

---

# Object Lifecycle

Every object follows the same lifecycle.

```
Create

↓

Initialize

↓

Use

↓

Destroy
```

---

# Constructor

Objects are created explicitly.

```c
Memory* memory_create(...);
```

---

# Destructor

Every created object has exactly one destroy function.

```c
memory_destroy(memory);
```

Memory ownership must always be obvious.

---

# Initialization

Complex initialization should be separated.

```c
memory_create();

memory_init();
```

instead of

```c
memory_create_everything();
```

---

# Ownership

Every function documents ownership.

Possible ownership models.

---

## Borrow

Caller owns memory.

Function temporarily uses it.

---

## Transfer

Ownership moves to the function.

Caller must not free it afterwards.

---

## Shared

Reference counting.

---

## Copy

Function copies data.

Caller remains owner.

---

Ownership must be documented for every parameter.

---

# Const Correctness

Everything that is not modified must be const.

Good

```c
void memory_lookup(
    Memory* memory,
    const char* key);
```

Bad

```c
void memory_lookup(
    Memory* memory,
    char* key);
```

---

# Immutable Data

Knowledge Objects should be immutable whenever possible.

Updating knowledge creates a new version.

Instead of

```
Version 5

↓

Modify

↓

Version 5
```

Use

```
Version 5

↓

Version 6
```

---

# Return Values

Functions return explicit status.

Never hide errors.

Preferred

```c
typedef enum {

SUCCESS,

NOT_FOUND,

INVALID_ARGUMENT,

OUT_OF_MEMORY,

TIMEOUT

} Status;
```

Example

```c
Status memory_insert(...);
```

---

# Output Parameters

Avoid multiple return values.

Use output parameters.

```c
Status memory_find(

Memory* memory,

const char* id,

Knowledge** result);
```

---

# Error Handling

Errors are values.

Not exceptions.

Never terminate the program inside a library.

Never call

```
exit()

abort()

panic()
```

inside reusable modules.

---

# Validation

Every public API validates its arguments.

Example

```c
if (memory == NULL)
    return INVALID_ARGUMENT;
```

Never trust the caller.

---

# Thread Safety

Thread safety must be documented.

Possible states.

```
Thread Safe

Read Only

Single Thread

External Lock Required
```

---

# Reentrancy

Functions should be reentrant whenever possible.

Avoid global state.

---

# Global Variables

Global mutable state is forbidden.

Allowed.

```
const tables

compile-time constants
```

Forbidden.

```
global cache

global context

global planner

global runtime
```

---

# Configuration

Configuration is passed explicitly.

Bad

```c
planner_run();
```

Good

```c
planner_run(planner, config);
```

---

# Memory Allocation

Allocation strategy must be obvious.

Example

```c
knowledge_create()

knowledge_destroy()
```

Never allocate hidden memory.

---

# Logging

Libraries never print directly.

Forbidden

```c
printf()

fprintf()

puts()
```

Instead

```
Logger API
```

---

# Callbacks

Callbacks must be optional.

Never require callbacks for basic functionality.

---

# Events

Long-running operations should publish events.

Example

```
Parsing Started

↓

Progress

↓

Finished
```

---

# Versioning

Every public API must support versioning.

Example

```
API v1

↓

API v2
```

Breaking changes require a new major version.

---

# Documentation

Every public function must document.

- purpose;
- parameters;
- ownership;
- return value;
- errors;
- thread safety.

---

Example

```c
/**
 * Stores a Knowledge Object.
 *
 * Ownership:
 * Borrow
 *
 * Thread Safety:
 * External Lock Required
 *
 * Returns:
 * Status
 */
Status memory_insert(...);
```

---

# Testing

Every public function must have tests.

Minimum.

- success;
- invalid arguments;
- edge cases;
- memory failures;
- concurrency if applicable.

---

# Performance

Public APIs must avoid.

- unnecessary allocation;
- hidden copies;
- quadratic algorithms;
- blocking operations.

Performance should be predictable.

---

# Binary Compatibility

Future versions should preserve ABI whenever practical.

Opaque structures help maintain compatibility.

---

# API Review Checklist

Before adding any new public API ask:

- Is the name explicit?
- Is ownership obvious?
- Is memory safe?
- Are errors explicit?
- Is thread safety documented?
- Is implementation hidden?
- Can it be tested?
- Can it evolve without breaking users?
- Does it belong in this module?
- Is there already an API that solves this?

If any answer is "no", redesign the API.

---

# API Invariants

Every public API must satisfy these rules.

- One responsibility per function.
- Explicit ownership.
- Explicit errors.
- Hidden implementation.
- Stable behavior.
- Minimal interface.
- No global mutable state.
- No hidden allocations.
- No direct printing.
- Testable.
- Documented.
- Predictable.

---

# Final Principle

A good API should feel obvious.

A developer should be able to discover how to use a subsystem by reading only its public header.

If documentation is required to understand a simple API, the API is probably poorly designed.

The implementation should be complex.

The API should appear simple.
