# State Machine

Version: 1.0

---

# Purpose

This document defines all finite state machines (FSM) used by NeuroCore.

Every subsystem with a lifecycle must be implemented as an explicit state machine.

Implicit states are forbidden.

State transitions must always be deterministic.

If an implementation allows a transition that is not defined in this document, the implementation is incorrect.

---

# Philosophy

Everything that changes over time has a state.

Every state:

- has a unique name;
- has defined entry conditions;
- has defined exit conditions;
- allows only valid transitions.

No object may exist "between states".

---

# General Rules

Every state machine must satisfy:

- deterministic transitions;
- no hidden states;
- explicit failure states;
- explicit recovery paths;
- complete transition table.

Every transition must generate an Event.

---

# Common Lifecycle

Most NeuroCore objects follow this lifecycle.

```
Created

↓

Initialized

↓

Ready

↓

Running

↓

Paused

↓

Running

↓

Stopping

↓

Stopped
```

Failure path:

```
Running

↓

Failed

↓

Recovering

↓

Running
```

or

```
Running

↓

Failed

↓

Stopped
```

---

# Runtime State Machine

Purpose:

Controls the lifecycle of the entire system.

```
Created

↓

Initializing

↓

Loading Configuration

↓

Starting Components

↓

Ready

↓

Running

↓

Stopping

↓

Shutdown

↓

Terminated
```

Failure path

```
Running

↓

Critical Failure

↓

Recovery

↓

Running
```

or

```
Running

↓

Critical Failure

↓

Emergency Shutdown
```

---

Allowed transitions

```
Created
    ↓
Initializing

Initializing
    ↓
Loading Configuration

Loading Configuration
    ↓
Starting Components

Starting Components
    ↓
Ready

Ready
    ↓
Running

Running
    ↓
Stopping

Stopping
    ↓
Shutdown

Shutdown
    ↓
Terminated
```

---

# VM State Machine

Purpose:

Represents active cognition.

```
Idle

↓

Thinking

↓

Planning

↓

Executing

↓

Learning

↓

Idle
```

Additional states

```
Waiting

Paused

Cancelled

Failed
```

Example

```
Idle

↓

Thinking

↓

Planning

↓

Executing

↓

Learning

↓

Idle
```

---

# Memory State Machine

```
Offline

↓

Loading

↓

Ready

↓

Searching

↓

Updating

↓

Saving

↓

Ready
```

Failure

```
Updating

↓

Error

↓

Recovery

↓

Ready
```

---

# Knowledge Object State Machine

Every Knowledge Object follows:

```
Created

↓

Validated

↓

Stored

↓

Indexed

↓

Active

↓

Archived
```

Deletion

```
Archived

↓

Deleted
```

Deletion should be extremely rare.

Archiving is preferred.

---

# Goal State Machine

```
Created

↓

Queued

↓

Active

↓

Planning

↓

Executing

↓

Completed
```

Alternative paths

```
Executing

↓

Failed
```

or

```
Queued

↓

Cancelled
```

---

# Plan State Machine

```
Created

↓

Validated

↓

Optimized

↓

Approved

↓

Executing

↓

Completed
```

Alternative

```
Validated

↓

Rejected
```

---

# Task State Machine

```
Created

↓

Ready

↓

Running

↓

Completed
```

Possible transitions

```
Running

↓

Waiting

↓

Running
```

```
Running

↓

Paused

↓

Running
```

```
Running

↓

Failed
```

---

# Thought State Machine

```
Created

↓

Scheduled

↓

Processing

↓

Finished
```

Failure

```
Processing

↓

Rejected
```

Thoughts never return to Created.

---

# Planner State Machine

```
Idle

↓

Receiving Goal

↓

Searching

↓

Building Plan

↓

Optimizing

↓

Validating

↓

Ready
```

Failure

```
Validating

↓

Rejected

↓

Searching
```

---

# Reasoning State Machine

```
Idle

↓

Selecting Strategy

↓

Reasoning

↓

Generating Conclusions

↓

Verification

↓

Finished
```

If verification fails

```
Verification

↓

Alternative Strategy

↓

Reasoning
```

---

# Learning State Machine

```
Idle

↓

Receiving Episode

↓

Analysis

↓

Pattern Discovery

↓

Knowledge Update

↓

Statistics Update

↓

Finished

↓

Idle
```

---

# Execution State Machine

```
Waiting

↓

Preparing

↓

Executing

↓

Collecting Results

↓

Episode Creation

↓

Finished
```

Alternative

```
Executing

↓

Tool Failure

↓

Recovery

↓

Executing
```

---

# Tool State Machine

```
Registered

↓

Initialized

↓

Available

↓

Executing

↓

Available
```

Failure

```
Executing

↓

Failed

↓

Restart

↓

Available
```

---

# Plugin State Machine

```
Discovered

↓

Loaded

↓

Initialized

↓

Running
```

Alternative

```
Running

↓

Reloading

↓

Running
```

or

```
Running

↓

Unloaded
```

---

# IPC Message State Machine

```
Created

↓

Queued

↓

Delivered

↓

Processing

↓

Completed
```

Alternative

```
Queued

↓

Expired
```

or

```
Processing

↓

Failed

↓

Retry

↓

Queued
```

---

# Event State Machine

```
Created

↓

Validated

↓

Published

↓

Delivered

↓

Archived
```

Events are immutable.

No transition allows modification.

---

# Episode State Machine

```
Recording

↓

Completed

↓

Stored

↓

Learning

↓

Archived
```

Episode content never changes after completion.

---

# Error State Machine

```
Detected

↓

Classified

↓

Reported

↓

Handled

↓

Archived
```

Unhandled errors become Critical Events.

---

# Recovery State Machine

```
Failure

↓

Diagnosis

↓

Recovery

↓

Verification

↓

Success
```

Alternative

```
Verification

↓

Failure

↓

Shutdown
```

---

# Allowed Transition Rules

General rules:

- transitions are one-way;
- backward transitions require explicit Recovery;
- Finished states cannot become Running again;
- immutable objects cannot return to editable states.

---

# Forbidden Transitions

Examples

Forbidden

```
Completed

↓

Running
```

Forbidden

```
Archived

↓

Created
```

Forbidden

```
Deleted

↓

Stored
```

Forbidden

```
Finished

↓

Executing
```

If such a transition is needed, a new object must be created.

---

# Transition Events

Every transition produces an Event.

Example

```
GoalCreated

GoalActivated

GoalCompleted

GoalFailed
```

State changes must always be observable.

---

# Persistence

Persistent objects save their current state.

Runtime-only states are never stored.

Persisted:

- Knowledge
- Goal
- Episode
- Plan

Not persisted:

- VM internal state
- Scheduler queues
- Temporary contexts

---

# Validation

Every transition is validated before execution.

Validation checks:

- current state;
- target state;
- transition rules;
- permissions;
- object integrity.

Invalid transitions are rejected.

---

# State Invariants

Every state machine guarantees:

- exactly one active state;
- deterministic transitions;
- explicit recovery;
- explicit terminal states;
- observable transitions;
- validation before transition;
- immutable history.

---

# Final Principle

State Machines define the behavior of NeuroCore.

The Data Model defines **what the system knows**.

The Event Model defines **what happens inside the system**.

The State Machine defines **how the system evolves over time**.

Together, these three specifications form the behavioral foundation of the entire NeuroCore architecture.
