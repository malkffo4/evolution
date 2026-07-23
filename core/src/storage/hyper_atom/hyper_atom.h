// storage/hyper_atom/hyper_atom.h
#ifndef HYPER_ATOM_H
#define HYPER_ATOM_H

#include <stdint.h>
#include <stddef.h>
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


typedef union {
    ko_id_t raw;        // Для побитовых масок и проверок
    ko_id_t ref;        // Чистый ID при HYPER_TYPE_REF
    double  f_val;      // Требует аккуратной упаковки без потери старших битов NaN
    int64_t i_val;
    char    s_val[8];   // Универсальное значение (8 байт)
} HyperVal;

// 64-байтная структура AGI-Атома
typedef struct __attribute__((packed)) {
    ko_id_t  id;
    ko_id_t  process_id;
    HyperVal args[3];
    ko_id_t  context_id;
    uint64_t time_tick;
    ko_id_t  cause_id;
} HyperAtom;

// Абстрактный интерфейс памяти
typedef struct HyperMemory HyperMemory;

HyperMemory *hyper_memory_new(MDB_txn *txn, MDB_dbi atoms, MDB_dbi idx_proc, MDB_dbi idx_args, MDB_dbi idx_ctx);
int hyper_assert(HyperMemory *mem, const HyperAtom *atom);
int hyper_assert_unique(HyperMemory *mem, const HyperAtom *atom);
int hyper_find_by_process(HyperMemory *mem, ko_id_t process_id, ko_id_t context_id, HyperAtom **results, size_t *count);
int hyper_find_by_participant(HyperMemory *mem, ko_id_t participant_id, ko_id_t context_id, HyperAtom **results, size_t *count);
int hyper_trace_cause(HyperMemory *mem, ko_id_t start_id, HyperAtom **chain, size_t max_depth, size_t *count);
void hyper_memory_free(HyperMemory *mem);

#endif // HYPER_ATOM_H
