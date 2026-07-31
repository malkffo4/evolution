// storage/hyper_atom/hyper_pattern.h
#pragma once

#include <lmdb.h>
#include "storage/hyper_atom/hyper_atom.h"

#define MAX_PATTERN_CONDITIONS 8
#define MAX_PATTERN_VARS       16
#define PATTERN_ARG_SLOTS      HYPER_VAL_SLOTS

typedef enum {
    PARG_CONST = 0,
    PARG_VAR   = 1,
    PARG_ANY   = 2
} PatternArgKind;

typedef struct {
    ko_id_t process_id;
    PatternArgKind kind[PATTERN_ARG_SLOTS];
    ko_id_t value[PATTERN_ARG_SLOTS];
    uint8_t var_index[PATTERN_ARG_SLOTS];
} PatternCondition;

typedef struct {
    ko_id_t  id;
    uint32_t condition_count;
    uint32_t var_count;
    PatternCondition conditions[MAX_PATTERN_CONDITIONS];
} HyperPattern;

int hyper_pattern_save(MDB_txn *txn, MDB_dbi dbi, const HyperPattern *pattern);
int hyper_pattern_load(MDB_txn *txn, MDB_dbi dbi, ko_id_t pattern_id, HyperPattern *out);
