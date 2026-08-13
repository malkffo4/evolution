// core/src/knowledge/claim_validator.c
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "claim_validator.h"
#include "math/hash.h"
#include "knowledge/evaluation.h"
#include "runtime/logging/logging.h"

#define CLAIM_CYCLE_DEFAULT_DEPTH 16

// Bounded DFS по локальному fan-out (idx_args, не полный скан отношения —
// тот же приём, что в composition_ops.c::vm_op_find_producer_chain).
static bool reaches(MDB_txn *txn, HyperMemory *hmem, node_id_t ordering_process_id,
                     node_id_t from, node_id_t to, uint32_t max_depth) {
    if (from == to) return true;

    node_id_t stack[CLAIM_CYCLE_DEFAULT_DEPTH];
    node_id_t visited[CLAIM_CYCLE_DEFAULT_DEPTH];
    uint32_t sp = 0, visited_count = 0, steps = 0;
    stack[sp++] = from;

    while (sp > 0 && steps < max_depth) {
        node_id_t cur = stack[--sp];
        steps++;

        bool seen = false;
        for (uint32_t i = 0; i < visited_count; i++)
            if (visited[i] == cur) { seen = true; break; }
        if (seen) continue;
        if (visited_count < CLAIM_CYCLE_DEFAULT_DEPTH) visited[visited_count++] = cur;

        NeuroAtom *edges = NULL;
        size_t count = 0;
        if (hyper_find_by_process(txn, hmem, ordering_process_id, cur, 0, &edges, &count) == 0) {
            for (size_t i = 0; i < count; i++) {
                if (HYPER_GET_ID(edges[i].args[0].raw) != cur) continue; // только исходящие
                node_id_t next = HYPER_GET_ID(edges[i].args[1].raw);
                if (next == to) { free(edges); return true; }
                if (sp < CLAIM_CYCLE_DEFAULT_DEPTH) stack[sp++] = next;
            }
            free(edges);
        }
    }
    return false;
}

static NeuroAtom *find_reverse_claim(MDB_txn *txn, HyperMemory *hmem,
                                      node_id_t ordering_process_id,
                                      node_id_t source, node_id_t target) {
    NeuroAtom *edges = NULL;
    size_t count = 0;
    static NeuroAtom found; // копия наружу, как в evaluation.c::find_score_atom
    if (hyper_find_by_process(txn, hmem, ordering_process_id, target, 0, &edges, &count) != 0)
        return NULL;
    for (size_t i = 0; i < count; i++) {
        if (HYPER_GET_ID(edges[i].args[0].raw) == target &&
            HYPER_GET_ID(edges[i].args[1].raw) == source) {
            memcpy(&found, &edges[i], sizeof(NeuroAtom));
            free(edges);
            return &found;
        }
    }
    free(edges);
    return NULL;
}

ClaimVerdict claim_validate_and_assert(MDB_txn *txn, HyperMemory *hmem,
                                        NeuroAtom *claim, ko_id_t cause_id,
                                        node_id_t ordering_process_id,
                                        uint32_t max_cycle_depth,
                                        node_id_t *out_conflict_id) {
    if (!txn || !hmem || !claim) return CLAIM_REJECTED_CYCLE;
    if (max_cycle_depth == 0 || max_cycle_depth > CLAIM_CYCLE_DEFAULT_DEPTH)
        max_cycle_depth = CLAIM_CYCLE_DEFAULT_DEPTH;

    node_id_t source = HYPER_GET_ID(claim->args[0].raw);
    node_id_t target = HYPER_GET_ID(claim->args[1].raw);

    if (claim->process_id == ordering_process_id &&
        reaches(txn, hmem, ordering_process_id, target, source, max_cycle_depth)) {
        LOG_REASONER("[CLAIM] REJECTED cycle: %lu -> %lu closes a loop in ordering %lu",
                     (unsigned long)source, (unsigned long)target,
                     (unsigned long)ordering_process_id);
        if (out_conflict_id) *out_conflict_id = target;
        return CLAIM_REJECTED_CYCLE;
    }

    NeuroAtom *reverse = find_reverse_claim(txn, hmem, ordering_process_id, source, target);
    bool conflict = reverse && reverse->truth_confidence > 0.6f;

    int rc = hyper_assert_unique(txn, hmem, claim);
    if (rc < 0) return CLAIM_REJECTED_CYCLE;
    if (rc == 1) return CLAIM_ACCEPTED_DUPLICATE;

    if (cause_id != 0)
        hyper_assert_with_cause(txn, hmem, claim, cause_id);

    score_update(txn, hmem, COGNITIVE_DOMAIN_CLAIM, claim->id, 1.0f, cause_id,
                 claim->context_or_time_link);

    if (conflict) {
        NeuroAtom contradicts = {0};
        contradicts.id = hyper_memory_new_id(hmem);
        contradicts.process_id = proc_make(djb2_hash("CONTRADICTS"), PROC_KIND_RELATION);
        contradicts.args[0].raw = HYPER_MAKE_REF(claim->id);
        contradicts.args[1].raw = HYPER_MAKE_REF(reverse->id);
        contradicts.truth_mean = 1.0f;
        contradicts.truth_confidence = 1.0f;
        contradicts.sti = 0.6f;
        contradicts.lti = 0.3f;
        hyper_assert_with_cause(txn, hmem, &contradicts, claim->id);

        // Понижаем доверие обеих версий, не удаляя ничего.
        score_update(txn, hmem, COGNITIVE_DOMAIN_CLAIM, claim->id, 0.3f, contradicts.id, 0);
        score_update(txn, hmem, COGNITIVE_DOMAIN_CLAIM, reverse->id, 0.3f, contradicts.id, 0);

        LOG_REASONER("[CLAIM] FLAGGED contradiction: %lu vs existing %lu",
                     (unsigned long)claim->id, (unsigned long)reverse->id);
        if (out_conflict_id) *out_conflict_id = reverse->id;
        return CLAIM_FLAGGED_CONTRADICTION;
    }

    return CLAIM_ACCEPTED;
}
