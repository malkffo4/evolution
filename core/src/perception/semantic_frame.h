// Fillmore Frames

#ifndef SEMANTIC_CORE_H
#define SEMANTIC_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include <lmdb.h>

#include "semantic.h"
#include "cognition.h"

typedef enum {
    SEM_ROLE_AGENT,
    SEM_ROLE_PATIENT,
    SEM_ROLE_THEME,
    SEM_ROLE_EXPERIENCER,
    SEM_ROLE_STIMULUS,
    SEM_ROLE_BENEFICIARY,
    SEM_ROLE_RECIPIENT,
    SEM_ROLE_SOURCE,
    SEM_ROLE_GOAL,
    SEM_ROLE_PATH,
    SEM_ROLE_LOCATION,
    SEM_ROLE_TIME,
    SEM_ROLE_INSTRUMENT,
    SEM_ROLE_MANNER,
    SEM_ROLE_CAUSE,
    SEM_ROLE_PURPOSE,
    SEM_ROLE_RESULT,
    SEM_ROLE_ATTRIBUTE,
    SEM_ROLE_OWNER,
    SEM_ROLE_VALUE,
    SEM_ROLE_DIRECTION,
    SEM_ROLE_ORIGIN,
    SEM_ROLE_DESTINATION,
    SEM_ROLE_CONDITION
} SemanticRole;

typedef struct {
    uint64_t id;
    uint64_t name_hash;
    semantic_hash_t semantics;
    SemanticRole role;
} SemanticEntity;

// typedef struct {
//     uint64_t event;
//     uint64_t actor;
//     uint64_t action;
//     uint64_t object;
//     uint64_t location;
//     uint64_t time;
//     uint64_t instrument;
//     uint64_t cause;
//     uint64_t intention;
//     uint64_t result;
// } SemanticFrame;

#define SEM_MAX_ROLES 32

typedef struct {
    SemanticRole role;
    node_id_t node;
} SemanticSlot;

typedef struct {
    node_id_t frame_id;

    node_id_t predicate;

    SemanticSlot slots[SEM_MAX_ROLES];

    uint32_t slot_count;

} SemanticFrame;

int semantic_enrich_graph(MDB_txn *txn, const char *json_str);

#endif // SEMANTIC_CORE_H
