// reasoning/strategy.h
#ifndef REASONING_STRATEGY_H
#define REASONING_STRATEGY_H

#include <stdbool.h>
#include "types/id.h"

typedef enum {
    EDGE_MATCH_GREEDY,
    EDGE_MATCH_HUNGARIAN,
    EDGE_MATCH_AUCTION
} EdgeMatchingAlgorithm;

typedef enum {
    COVERAGE_MAX,
    COVERAGE_F1,
    COVERAGE_JACCARD
} CoverageAlgorithm;

typedef enum {
    RELATION_EXACT,
    RELATION_ONTOLOGY,
    RELATION_EMBEDDING
} RelationAlgorithm;

typedef struct {
    float neighborhood;
    float center;
    float coverage;
    float relation;
} ReasoningWeights;

typedef struct {
    float min_similarity;
    float min_coverage;
    bool strict;
} ReasoningThresholds;

typedef struct {
    uint64_t executions;
    uint64_t success;
    uint64_t failures;
    float average_score;
    float average_runtime_ms;
} ReasoningStatistics;

typedef struct {
    strategy_id_t id;
    char name[64];
    EdgeMatchingAlgorithm edge_matching;
    CoverageAlgorithm coverage;
    RelationAlgorithm relation;
    ReasoningWeights weights;
    ReasoningThresholds thresholds;
    ReasoningStatistics statistics;
} ReasoningStrategy;

#endif
