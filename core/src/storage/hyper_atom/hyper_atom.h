// storage/hyper_atom/hyper_atom.h
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <lmdb.h>

// 64-битный универсальный идентификатор
typedef uint64_t ko_id_t;

// --- СИСТЕМА ТИПИЗАЦИИ АРГУМЕНТОВ (2 СТАРШИХ БИТА) ---
// Используем верхние 2 бита ko_id_t. Оставшиеся 62 бита - это чистый ID/Значение.
#define HYPER_TYPE_MASK   0xC000000000000000ULL
#define HYPER_VALUE_MASK  0x3FFFFFFFFFFFFFFFULL

#define HYPER_TYPE_REF    0x0000000000000000ULL // 00: Ссылка на объект/атом
#define HYPER_TYPE_INT    0x4000000000000000ULL // 01: Целое число (до 62 бит)
#define HYPER_TYPE_FLOAT  0x8000000000000000ULL // 10: Float (упакованный)
#define HYPER_TYPE_STR    0xC000000000000000ULL // 11: Короткая строка (Inline)

// Макросы для работы с аргументами
#define HYPER_GET_TYPE(val) ((val) & HYPER_TYPE_MASK)
#define HYPER_GET_ID(val)   ((val) & HYPER_VALUE_MASK)
#define HYPER_MAKE_REF(id)  (((id) & HYPER_VALUE_MASK) | HYPER_TYPE_REF)

// --- МАСКА ТИПА УЗЛА В process_id (старшие 8 бит зарезервированы) ---
#define PROC_TYPE_SHIFT      56
#define PROC_TYPE_MASK       (0xFFULL << PROC_TYPE_SHIFT)
#define PROC_ID_MASK         (~PROC_TYPE_MASK)

#define HYPER_VAL_SLOTS      2

#define VECTOR_DIM 128

typedef enum {
    PROC_KIND_RELATION = 0,  // связь (IS_A, CAUSES, ...)
    PROC_KIND_ENTITY   = 1,  // любое понятие, включая мета-категории
    PROC_KIND_RULE     = 2,  // правило вывода (IF..THEN)
    PROC_KIND_EVENT    = 3,  // событие во времени
    PROC_KIND_GOAL     = 4   // цель агента
    // Всё остальное (SKILL, PREDICTION, BELIEF, ...) уходит в граф.
} ProcKind;

static inline ko_id_t proc_make(ko_id_t base_id, ProcKind kind) {
    return (base_id & PROC_ID_MASK) | ((ko_id_t)kind << PROC_TYPE_SHIFT);
}
static inline ProcKind proc_kind(ko_id_t process_id) {
    return (ProcKind)((process_id & PROC_TYPE_MASK) >> PROC_TYPE_SHIFT);
}

typedef union {
    ko_id_t raw;
    ko_id_t ref;
    double  f_val;
    int64_t i_val;
    char    s_val[8];
} HyperVal;

typedef struct {
    float data[VECTOR_DIM];
} Vector128;

/*
 * NeuroAtom — когнитивная триада: Truth (эпистемика) + Attention (внимание)
 * + Utility/Valence (телеология/аффект). Строго 64 байта, C11, без padding'а
 * при 8-байтном выравнивании uint64_t/double.
 *
 * Layout (offset : size):
 *   0  : id                    (8)
 *   8  : process_id            (8)
 *   16 : args[0]                (8)
 *   24 : args[1]                (8)
 *   32 : truth_mean             (4)
 *   36 : truth_confidence       (4)
 *   40 : sti                    (4)
 *   44 : lti                    (4)
 *   48 : utility                (4)
 *   52 : valence                (4)
 *   56 : context_or_time_link   (8)
 *   = 64 bytes total
 */
typedef struct {
    ko_id_t  id;                     // 8
    ko_id_t  process_id;             // 8  (старший байт = ProcKind)
    HyperVal args[HYPER_VAL_SLOTS];                // 16 (было 3 — теперь бинарные отношения)

    // --- Epistemic Vector (PLN-style) ---
    float truth_mean;                // 4  0.0..1.0 — насколько это истинно
    float truth_confidence;          // 4  0.0..1.0 — насколько мы уверены в оценке

    // --- Attentional Vector ---
    float sti;                       // 4  Short-Term Importance (контекст/фокус)
    float lti;                       // 4  Long-Term Importance (выживание в памяти)

    // --- Teleological / Affective Vector ---
    float utility;                   // 4  0.0..1.0 — полезность для текущих целей
    float valence;                   // 4  -1.0..1.0 — "эмоция": опасно/безопасно

    ko_id_t context_or_time_link;    // 8  ссылка на контекст/временную обёртку
} NeuroAtom;

_Static_assert(sizeof(NeuroAtom) == 64, "NeuroAtom must be exactly 64 bytes");
_Static_assert(_Alignof(NeuroAtom) == 8, "NeuroAtom must be 8-byte aligned for LMDB mmap access");

typedef uint16_t HyperNodeId;
typedef uint16_t HyperSessionId;

typedef struct {
    HyperNodeId    node_id;
    HyperSessionId session_id;
    _Atomic uint32_t counter;
} HyperIdGenerator;

typedef struct HyperMemory {
    MDB_txn *txn;

    MDB_dbi dbi_atoms;
    MDB_dbi dbi_idx_process;
    MDB_dbi dbi_idx_args;
    MDB_dbi dbi_idx_context;
    MDB_dbi dbi_idx_causal;   // child_id -> cause_id (DUPSORT)
    MDB_dbi dbi_archive;      // холодное хранилище архивных атомов
    MDB_dbi dbi_idx_vectors;

    HyperIdGenerator *idgen;
} HyperMemory;

ko_id_t hyper_memory_new_id(HyperMemory *mem);

HyperMemory *hyper_memory_new(MDB_txn *txn, MDB_dbi atoms, MDB_dbi idx_proc, MDB_dbi idx_args, MDB_dbi idx_ctx);
void hyper_memory_free(HyperMemory *mem);
void hyper_memory_set_txn(HyperMemory *mem, MDB_txn *txn);
void hyper_memory_set_db_archive(HyperMemory *mem, MDB_dbi archive);
void hyper_memory_set_db_causal(HyperMemory *mem, MDB_dbi causal);
void hyper_memory_set_db_vectors(HyperMemory *mem, MDB_dbi vectors);

int hyper_assert(HyperMemory *mem, const NeuroAtom *atom);
int hyper_assert_unique(HyperMemory *mem, const NeuroAtom *atom);
int hyper_assert_with_cause(HyperMemory *mem, const NeuroAtom *atom, ko_id_t cause_id);

int hyper_find_by_process(HyperMemory *mem, ko_id_t process_id, ko_id_t participant_id, ko_id_t context_id, NeuroAtom **results, size_t *count);
int hyper_find_by_participant(HyperMemory *mem, ko_id_t participant_id, ko_id_t context_id, NeuroAtom **results, size_t *count);

// Взвешенный по STI/Utility поиск — для агентного цикла (см. п.4)
int hyper_find_top_by_score(HyperMemory *mem, ko_id_t context_id, float w_sti, float w_utility,
                             int top_k, NeuroAtom **results, size_t *count);

int hyper_trace_cause(HyperMemory *mem, ko_id_t start_id, NeuroAtom **chain, size_t max_depth, size_t *count);

// Сохранить/загрузить эмбеддинг для атома
int hyper_vector_save(MDB_txn *txn, MDB_dbi dbi, ko_id_t atom_id, const Vector128 *vec);
int hyper_vector_load(MDB_txn *txn, MDB_dbi dbi, ko_id_t atom_id, Vector128 *out);

// Косинусное сходство
float vector_cosine_similarity(const Vector128 *a, const Vector128 *b);

int hyper_find_by_process_sti(HyperMemory *mem, ko_id_t process_id, ko_id_t participant_id,
                               ko_id_t context_id, float sti_threshold,
                               NeuroAtom **results, size_t *count);
