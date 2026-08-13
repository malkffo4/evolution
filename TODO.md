Как избавиться от хардкода и двигаться к AGI

Архитектура системы уже находится на очень продвинутом уровне. Переход от "программы" к "системе, которая мыслит" требует полного отказа от зашитой в C логики в пользу графового представления (Code-as-Data).

Вот конкретные архитектурные шаги, которые органично лягут на твою текущую базу:
Шаг A: Перенос концептов из C-ядра в базу (Bootstrap)

Ты уже сам заметил проблему в коде:

    // TODO. Для системы, которая должна жить месяцами, учиться, исполнять алгоритмы и сохранять историю, я бы сделал...

Тот же принцип касается семантики. Сейчас в коде ядра (в планировщике, критике, памяти) разбросаны вызовы вроде djb2_hash("Goal"), djb2_hash("MainLoop"), djb2_hash("IS_A").
Ядро C вообще не должно знать человеческих слов.

    Решение: Вынеси все базовые концепты, операторы и начальные алгоритмы (включая MainLoop и CorePlanner) в bootstrap.json. При старте с пустой базой ядро должно просто "съесть" этот файл через perceive_hyper_json и запустить атом, который помечен флагом "Entrypoint".

Шаг B: Перевод алгоритмов в PROC_KIND_INSTRUCTION (Отказ от Pipeline)

В C-структурах Pipeline инструкции лежат линейно в памяти. Это мешает AGI их модифицировать.

    У тебя уже есть гениальный инструмент: OP_EVAL_GRAPH. Он позволяет исполнять граф атомов NeuroAtom как программу.

    Решение: Нужно полностью избавиться от Pipeline как сущности хранения. Алгоритмы должны компилироваться в цепочки графа (атомы с типом PROC_KIND_INSTRUCTION), связанные через idx_causal_rev. Тогда AGI сможет применять к своему собственному коду те же операторы OP_MINE_CAUSAL_PATTERN и OP_MATCH_PATTERN, которыми он изучает внешний мир. Код станет полноценной частью базы знаний.

Шаг C: Мета-правила вместо жестких политик (Отказ от C-логики для гомеостаза)

Правила затухания памяти (DecayPolicy) и карантина (critic_state) сейчас живут в C-коде.

    Решение: Используй PROC_KIND_RULE для создания атомов логического вывода в графе.

        Пример графового правила: IF (Atom.STI < 0.05 AND Atom.LTI < 0.1) THEN OP_ARCHIVE_ATOM(Atom.ID).

        Если критик понимает, что система "забыла" важный факт, он может понизить truth_confidence этому правилу, и система сама скорректирует порог затухания памяти без перекомпиляции C-кода.

Шаг D: Динамическая система Capability

Операторы сейчас жестко регистрируются в operator_registry_init(). Планировщик ищет их по CapabilityMask.

    Решение: Создай атомы Capability в графе. Свяжи алгоритмы/операторы с ними через отношение IMPLEMENTS_CAPABILITY. Тогда система сможет сама находить новые инструменты в интернете, писать для них Python-обертки, вызывать их через OP_TOOL_EXEC и динамически добавлять в свой арсенал планировщика новые узлы Capability.

    

# TODO.md
# Must
NeuroCore должен научиться на 20 sandbox-пентестах, а затем превзойти baseline LLM-agent на 80 совершенно новых targets без retraining.

## Priority 1
Credit Assignment
Сейчас Planner знает
получился успех
Но ещё не знает
КТО его сделал.
Это огромная разница.

## Priority 2
Knowledge Extraction
Не просто JSON → HyperAtom.
А
книга
↓
NER
↓
отношения
↓
HyperAtom
↓
Pattern
↓
Pipeline
То есть первая настоящая обучалка.

## Priority 3
Planner Exploration
Сейчас
выбрать лучший
Нужно
выбрать лучший
ИЛИ
попробовать новый
Иначе обучение остановится.

## Priority 4
Replay
Во сне.
эпизоды
↓
переиграть
↓
новые гипотезы
↓
новые правила
Это очень сильный механизм.

Priority 5
Distributed Writer
Сейчас уже видно, что вы придёте к
VM1
VM2
VM3
VM4
↓
Writer Queue
↓
LMDB
Вместо
каждая VM сама пишет.
абстрагировать MDB_txn от ipc и т.д.
Транзакции LMDB и многопоточность
Где: core/src/vm/vm_context.h (использование MDB_txn *txn)
Проблема:
LMDB имеет строгое правило: транзакция (Transaction) жестко привязана к потоку (Thread), который её создал. Если мы создаем транзакцию в одном потоке, а VMContext передается в другой, или если два IPC-потока одновременно начнут писать в граф без синхронизации транзакций уровня записи, база данных выдаст ошибку или заблокируется.
Решение:
Четко разделить пулы транзакций. Очередь записи в Граф должна быть атомарной. Все MDB_RDONLY (чтение) могут идти параллельно из любого потока, но MDB_CREATE/write должен происходить либо под строгим мьютексом, либо отправляться в выделенный поток записи (Writer Thread).

# Critic
минимизировать C-код Critic до инфраструктуры

Это правильное архитектурное решение. CriticMain должен быть таким же алгоритмом, как CheckEdgeAlgo, сохранённым через pipeline_import_json. VM исполняет его в общей очереди.

Порядок действий:
Зарегистрируйте новый оператор OP_READ_FAILURES, который читает атомы EXEC_FAILED и складывает их в scratchpad.
Напишите алгоритм CriticMain (пока в Python для прототипа, потом как JSON-пайплайн), который:
читает failures через OP_READ_FAILURES
для каждого failure создаёт атом CONFIDENCE_DELTA с отрицательным весом
если failures > 3 для одного algo_id, создаёт атом HAS_FLAW
Добавьте CriticMain в MainLoop после OP_EVALUATE_GOALS.

Безопасное исполнение: так как CriticMain — обычный алгоритм, VM выполняет его в том же потоке, что и MainLoop. Никаких гонок. Если он станет тяжёлым, можно вынести в отдельный поток с копией гипер-памяти (read-only транзакция + запись через очередь).

# Интеграция с OS
## **Для этого НУЖНО ввести в базу список безопасных, опасных операций, както заложить смыслы, намерения, желания**
- Permission System
- Capability System
- Sandbox
- Resource limits
Чтобы алгоритмы из БД могли читать файлы или работать с сетью, тебе нужно добавить в opcode.h всего одну инструкцию — например, OP_SYSCALL.
Аргументы Instruction будут маппиться на системные вызовы POSIX. Внутри пайплайна можно будет выполнить: [OP_SYSCALL, SYS_SOCKET, ...] или [OP_SYSCALL, SYS_OPEN, ...].
**Ввести систему разрешений (permissions) в контекст VM, чтобы не протестированные "фантазии" подсознания случайно не удалили что-то с жесткого диска**


# Самопроверка должна быть не одной функцией, а несколькими независимыми каналами
Для твоей цели я бы ввёл концепцию:
Claim
 ├── source evidence
 ├── derivation
 ├── execution evidence
 ├── verification evidence
 ├── counter-evidence
 └── confidence

Например алгоритм из книги:
Algorithm A
Core должен иметь возможность:
выполнить A;
получить результат;
проверить результат другим алгоритмом B;
проверить инварианты;
сравнить с известным примером;
сравнить с предыдущими эпизодами;
поискать контрпример;
только после этого обновить Score.

То есть:
execute(A)
     ↓
result
     ↓
verify(B)
     ↓
verify(invariants)
     ↓
compare(history)
     ↓
counterexample search
     ↓
confidence update

Это значительно сильнее простого:
outcome = rc == VM_OK ? 1 : 0;
который сейчас используется в VM pool.
Потому что VM_OK означает лишь "программа не упала", а не "алгоритм решил задачу правильно".
Это, на мой взгляд, одна из главных архитектурных вещей, которую сейчас надо менять.

# Сейчас outcome слишком примитивный

Сегодня:

float outcome = (rc == VM_OK) ? 1.0f : 0.0f;

То есть:
VM_OK → успех
VM_ERROR → провал

Но для AGI-подобного цикла нужно:
typedef struct {
    float execution_success;
    float result_validity;
    float verifier_agreement;
    float invariant_score;
    float reproducibility;
    float novelty;
    float usefulness;
    float safety;
} EvaluationVector;

И затем:
overall_outcome =
    weighted combination

Причём веса тоже могут обучаться.
Тогда:
алгоритм отработал без crash
не будет автоматически означать:
алгоритм хороший

# И ещё важнее: нужен distinction между "знанием" и "исполняемым знанием"

У тебя уже есть NodeType:

CONCEPT
EPISODE
RULE
ALGORITHM
PLAN
HYPOTHESIS
OBSERVATION
GOAL
SKILL
ERROR
LESSON

Это очень хорошая заготовка.

Я бы теперь жёстко разделил жизненный цикл:

CONCEPT
   ↓
CLAIM
   ↓
RULE
   ↓
HYPOTHESIS
   ↓
ALGORITHM
   ↓
SKILL
   ↓
PLAN
   ↓
EXECUTION
   ↓
OBSERVATION
   ↓
EVALUATION
   ↓
LESSON

А ошибка:

ERROR

должна быть связана обратно:

ERROR
 ├── caused_by → algorithm
 ├── occurred_in → episode
 ├── contradicts → claim
 └── lesson → modified strategy

Вот тогда Critic становится настоящим механизмом обучения, а не просто quarantine.

# Ключевой принцип: книга не должна сразу становиться "истиной"
Это особенно важно.
Допустим книга говорит:
Метод X решает задачу Y за O(n log n).
Система должна сохранить не:

X IS_TRUE

а примерно:
CLAIM(
    subject=X,
    relation=SOLVES,
    object=Y
)

CLAIM(
    subject=X,
    relation=COMPLEXITY,
    object=O(n log n)
)

EVIDENCE(
    claim=...
    source=Book42
    location=Chapter7/Page123
)

и отдельно:
CONFIDENCE(claim) = ...

Тогда Core может сказать:
"Это утверждение получено из книги, но я его ещё не проверял."

После выполнения примера:
EXPECTED_RESULT
       │
       ├── actual result
       │
       └── independent verifier

появляется новый OBSERVED_OUTCOME.
И только потом меняется доверие.

# Но сейчас система ещё не умеет "читать книгу и научиться выполнять её"

Сейчас learn фактически принимает структурированное знание.

Есть:

learn
 ├── pipeline
 ├── hyper atoms
 ├── nodes
 └── patterns

и pipeline импортируется напрямую в исполняемый Pipeline.

Это означает:

у тебя есть загрузчик знания, но нет полноценного Knowledge Compiler.

Для твоей цели нужен следующий конвейер:

КНИГА
  │
  ▼
Document
  │
  ├── главы
  ├── секции
  ├── абзацы
  ├── формулы
  ├── таблицы
  ├── примеры
  └── исходный текст
  │
  ▼
Knowledge Extraction
  │
  ├── concepts
  ├── relations
  ├── rules
  ├── procedures
  ├── algorithms
  ├── preconditions
  ├── expected outcomes
  └── evidence
  │
  ▼
Knowledge Graph + HyperAtoms
  │
  ▼
Executable Knowledge
  │
  ├── Rule
  ├── Algorithm
  ├── Skill
  └── Plan
  │
  ▼
Sandbox execution
  │
  ▼
Observation
  │
  ▼
Verification
  │
  ├── expected result
  ├── actual result
  ├── independent check
  └── contradiction search
  │
  ▼
Evaluation
  │
  ▼
Score / Confidence
  │
  ▼
Memory

Вот это и есть направление, в котором я бы развивал Core.


# Main Task
- Прочитать основы пентеста
- Запомнить по пунктам например модель Kill Chain
- Применить ее в полной мере на тестовой среде
