// storage/hyper_atom/hyper_pattern.h
#ifndef HYPER_PATTERN_H
#define HYPER_PATTERN_H

#include <lmdb.h>
#include "storage/hyper_atom/hyper_atom.h"

#define MAX_PATTERN_CONDITIONS 8
#define MAX_PATTERN_VARS       16

typedef enum {
    PARG_CONST = 0,  // конкретное значение (уже с HYPER_TYPE_* тегом)
    PARG_VAR   = 1,  // переменная паттерна
    PARG_ANY   = 2   // wildcard
} PatternArgKind;

typedef struct {
    ko_id_t process_id;
    PatternArgKind kind[3];
    ko_id_t value[3];      // valid при PARG_CONST
    uint8_t var_index[3];  // valid при PARG_VAR
} PatternCondition;

typedef struct {
    ko_id_t  id;
    uint32_t condition_count;
    uint32_t var_count;
    PatternCondition conditions[MAX_PATTERN_CONDITIONS];
} HyperPattern;

int hyper_pattern_save(MDB_txn *txn, MDB_dbi dbi, const HyperPattern *pattern);
int hyper_pattern_load(MDB_txn *txn, MDB_dbi dbi, ko_id_t pattern_id, HyperPattern *out);

#endif
