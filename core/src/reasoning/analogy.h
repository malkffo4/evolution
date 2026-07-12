// reasoning/analogy.h
#ifndef ANALOGY_H
#define ANALOGY_H

#include <lmdb.h>
#include "types/id.h"
#include "storage/edge/edge.h"

typedef struct {
    Triple query;
    Triple candidate;
    float similarity;
} EdgeMatch;

typedef struct {
    float incoming;
    float outgoing;
    float center;
    float coverage;
    float neighborhood;
    float relation;
    float total;
    float novetly;
    float creativity;
} AnalogyScore;

typedef struct {
    EdgeMatch best_incoming;
    EdgeMatch best_outgoing;
    AnalogyScore score;
} AnalogyEvaluation;

typedef struct {
    node_id_t query_node;
    node_id_t analogous_node;
    Triple query_condition;
    Triple analogous_condition;
    Triple query_result;
    Triple analogous_result;
    AnalogyScore score;
} AnalogyCandidate;

typedef struct
{
    EdgeMatch *matches;
    int count;
    float average_similarity;
    float relation_similarity;
    float coverage;
    float best_similarity;
} EdgeMatching;

typedef enum {
    ANALOGY_EXACT,
    ANALOGY_BALANCED,
    ANALOGY_CREATIVE,
    ANALOGY_EXTREME
} AnalogyMode;

// Функция для поиска аналогий по узлу: conditions -> goal -> results
int find_analogous_patterns(MDB_txn *txn, node_id_t node_id, AnalogyCandidate **candidates, int *candidate_count);

#endif // ANALOGY_H
