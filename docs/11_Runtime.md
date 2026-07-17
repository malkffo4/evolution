# Runtime

Version: 1.0

---

# Назначение

Runtime является главным управляющим компонентом NeuroCore.

Если VM отвечает за выполнение когнитивных процессов, то Runtime отвечает за жизнь всей системы.

Runtime создаёт систему.

Запускает систему.

Останавливает систему.

Следит за её состоянием.

Перезапускает компоненты при необходимости.

Runtime является "операционной системой" NeuroCore.

---

# Главная задача

Управлять жизненным циклом всех компонентов.

```
Start

↓

Initialize

↓

Run

↓

Monitor

↓

Recover

↓

Shutdown
```

Никакая подсистема не должна самостоятельно управлять своим жизненным циклом.

---

# Основной принцип

Каждый компонент NeuroCore является сервисом Runtime.

Runtime ничего не знает о внутренней реализации компонентов.

Он знает только их интерфейс.

```
Runtime

↓

Component Interface

↓

Concrete Component
```

Благодаря этому любой компонент можно заменить.

---

# Архитектура

```
                    Runtime

                        │

              Lifecycle Manager

                        │

      ┌─────────────────┼─────────────────┐

      │                 │                 │

 Component Manager   Event Bus    Resource Manager

      │                 │                 │

 Health Monitor    Configuration   Scheduler

      │                 │                 │

      └─────────────────┼─────────────────┘

                        │

                    All Modules
```

---

# Подсистемы Runtime

Runtime управляет следующими компонентами.

```
Memory

Understanding

Reasoning

Planner

Execution

Learning

VM

Tools

Plugins

IPC

Storage
```

Все они имеют одинаковый жизненный цикл.

---

# Жизненный цикл системы

Полный жизненный цикл выглядит следующим образом.

```
Process Start

↓

Load Configuration

↓

Initialize Runtime

↓

Initialize Components

↓

Dependency Check

↓

Start Components

↓

Ready

↓

Running

↓

Shutdown

↓

Cleanup

↓

Exit
```

---

# Boot Sequence

Порядок запуска компонентов является фиксированным.

```
Runtime

↓

Configuration

↓

Logger

↓

Storage

↓

Memory

↓

VM

↓

IPC

↓

Tools

↓

Understanding

↓

Reasoning

↓

Planner

↓

Execution

↓

Learning

↓

Plugins

↓

Ready
```

Причина именно такого порядка:

Каждый следующий компонент зависит только от уже запущенных.

---

# Component Interface

Каждый компонент обязан реализовать одинаковый интерфейс.

Минимально.

```
Init()

Start()

Stop()

Pause()

Resume()

Update()

Shutdown()

Health()

Status()
```

Runtime знает только эти методы.

---

# Component States

Каждый компонент может находиться только в одном состоянии.

```
Created

↓

Initialized

↓

Starting

↓

Running

↓

Paused

↓

Stopping

↓

Stopped

↓

Failed
```

Переходы между состояниями контролирует Runtime.

---

# Lifecycle Manager

Lifecycle Manager отвечает за изменение состояний компонентов.

Например

```
Init

↓

Start

↓

Pause

↓

Resume

↓

Shutdown
```

Никакой компонент не может изменить своё состояние самостоятельно.

---

# Dependency Manager

Перед запуском Runtime проверяет зависимости.

Например.

```
Reasoning

↓

Memory

↓

Storage
```

Если Memory не запущена,

Reasoning не запускается.

---

# Configuration

Все параметры системы находятся в едином Configuration.

Например

```
config.yaml

config.json

config.toml
```

Компоненты не читают файлы конфигурации самостоятельно.

Все параметры предоставляет Runtime.

---

# Resource Manager

Runtime отвечает за распределение ресурсов.

Например

- память;
- CPU;
- GPU;
- количество потоков;
- лимиты памяти;
- лимиты времени;
- очереди задач.

Компоненты не должны самостоятельно выделять бесконтрольные ресурсы.

---

# Health Monitor

Health Monitor постоянно проверяет состояние компонентов.

Например

```
Memory

Running

↓

Healthy
```

или

```
Planner

Timeout

↓

Unhealthy
```

---

# Heartbeat

Каждый компонент обязан периодически отправлять Heartbeat.

```
Component

↓

Heartbeat

↓

Runtime
```

Если Heartbeat отсутствует,

компонент считается зависшим.

---

# Recovery

Runtime способен автоматически восстанавливать компоненты.

Например

```
Execution

↓

Crash

↓

Restart

↓

Running
```

или

```
Plugin

↓

Failure

↓

Unload
```

---

# Graceful Shutdown

Завершение работы должно происходить строго по порядку.

```
Stop New Tasks

↓

Finish Active Tasks

↓

Flush Memory

↓

Save State

↓

Close Storage

↓

Shutdown Components

↓

Exit
```

Запрещается аварийно завершать процесс без необходимости.

---

# Runtime Events

Runtime получает события от всех компонентов.

Например

```
ComponentStarted

ComponentStopped

ComponentFailed

MemoryFull

TaskCreated

TaskFinished

PluginLoaded

PluginUnloaded
```

Все события передаются в IPC.

---

# Runtime Scheduler

Runtime имеет собственный Scheduler.

Он отвечает только за выполнение системных задач.

Например

- очистка памяти;
- архивирование;
- сохранение состояния;
- обслуживание индексов;
- фоновое обучение.

Scheduler Runtime не имеет отношения к Planner.

---

# Safe Mode

Если критический компонент не может быть запущен,

Runtime может перейти в Safe Mode.

Например

```
Storage unavailable

↓

Read Only Memory

↓

Limited Functionality
```

Или

```
LLM unavailable

↓

Disable LLM Adapter

↓

Continue Working
```

Архитектура должна деградировать постепенно.

---

# Hot Reload

Runtime должен поддерживать горячую замену некоторых компонентов.

Например

```
Plugin

↓

Unload

↓

Load New Version

↓

Continue
```

Без остановки всей системы.

---

# Crash Recovery

После аварийного завершения Runtime способен восстановить систему.

Например

- открыть LMDB;
- проверить журнал;
- восстановить незавершённые задачи;
- восстановить Working Memory;
- продолжить выполнение.

---

# Логирование

Runtime отвечает за централизованное логирование.

Минимальные категории.

```
INFO

WARNING

ERROR

DEBUG

TRACE

AUDIT
```

Компоненты не должны самостоятельно писать в произвольные файлы.

---

# Производительность

Runtime должен:

- иметь минимальные накладные расходы;
- не блокировать рабочие потоки;
- поддерживать многопоточность;
- поддерживать асинхронность;
- быть потокобезопасным.

---

# Инварианты

Runtime обязан гарантировать:

- корректный запуск компонентов;
- корректную остановку компонентов;
- единый жизненный цикл;
- централизованную конфигурацию;
- централизованное логирование;
- контроль зависимостей;
- контроль ресурсов;
- восстановление после ошибок;
- отсутствие циклических зависимостей между компонентами.

---

# Заключение

Runtime является фундаментом исполнения NeuroCore.

Он не занимается мышлением, хранением знаний или планированием.

Его задача — обеспечить стабильную, управляемую и отказоустойчивую работу всей когнитивной системы, координируя жизненный цикл каждого компонента и предоставляя единые механизмы управления ресурсами, конфигурацией и состоянием.
