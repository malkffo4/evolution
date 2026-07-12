// главный API. диспетчер reasoning
// reasoning/engine.h
//
#ifndef REASONING_H
#define REASONING_H

#include <stdbool.h>

#include "memory/working.h"

typedef struct
{
    bool enable_deduction;
    bool enable_induction;
    bool enable_abduction;
    bool enable_analogy;
    bool enable_contradiction;
} ReasonerConfig;

int reasoner_run(
        WorkingMemory *wm,
        MDB_txn *txn,
        const ReasonerConfig *cfg);

typedef struct
{
    MDB_txn *txn;

    WorkingMemory *wm;

} InferenceContext;

typedef struct
{
    MDB_txn *txn;

    WorkingMemory *wm;

    const ReasonerConfig *config;

    // PatternMatcher matcher;

    // HypothesisManager hypotheses;

} ReasoningContext;

// int inference_run(InferenceContext *ctx);
// int inference_step(...);

// int inference_deduce(...);
// int inference_induce(...);
// int inference_abduce(...);

// int inference_find_contradictions(...);
// int inference_generate_hypotheses(...);

// reasoning_engine(...) сам вызывает
// reasoner_step(...)
// {
//     deduction(...);

//     induction(...);

//     abduction(...);

//     analogy(...);

//     contradiction(...);

//     hypothesis(...);
// }

#endif // REASONING_H
