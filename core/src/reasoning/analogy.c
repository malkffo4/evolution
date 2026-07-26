// reasoning/analogy.c — переносит знания между похожими ситуациями.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "analogy.h"
#include "storage/db/db.h"
#include "storage/node/node.h"
#include "storage/graph/graph.h"
#include "storage/vector_store/vector_store.h"  // если нужно load_embedding
#include "math/vector_math.h"  // semantic_distance_u64
#include "reasoning/strategy.h"

// Forward declarations
static float edge_similarity(MDB_txn *txn, const Edge *e1, const Edge *e2);
static AnalogyEvaluation analogy_fast_score(
    MDB_txn *txn,
    node_id_t query_node,
    node_id_t candidate,
    EdgeList *in_edges,
    EdgeList *out_edges,
    EdgeList *cand_in,
    EdgeList *cand_out,
    const ReasoningStrategy *strategy);

// TODO vm_op_vector_sim переиспользовать вместо C-шной node_similarity().
// Простая метрика похожести двух узлов: 0..1
static float node_similarity(MDB_txn *txn, node_id_t a, node_id_t b) {
    if (a == b) return 1.0f;
    Node na, nb;
    if (get_node(txn, a, &na) != 0 || get_node(txn, b, &nb) != 0) return 0.0f;
    uint64_t ha = na.simhash;
    uint64_t hb = nb.simhash;
    if (ha != 0 && hb != 0) {
        int dist = semantic_distance_u64(ha, hb);
        return 1.0f - (float)dist / 64.0f;
    }
    if (na.name_hash == nb.name_hash) return 1.0f;
    return 0.0f;
}

static inline float relation_similarity(relation_id_t a, relation_id_t b) {
    return (a == b);
}

// greedy matching (позже можно заменить на Hungarian)
static int match_edges(MDB_txn *txn, EdgeList *query, EdgeList *candidate,
                       EdgeMatch **matches, int *count) {
    *matches = NULL;
    *count = 0;
    if (query->count == 0 || candidate->count == 0)
        return MDB_SUCCESS;

    EdgeMatch *result = calloc(query->count, sizeof(EdgeMatch));
    if (!result)
        return ENOMEM;

    char *used = calloc(candidate->count, sizeof(char));
    if (!used) {
        free(result);
        return ENOMEM;
    }

    int matched = 0;
    for (uint32_t i = 0; i < query->count; i++) {
        float best = 0.f;
        int best_j = -1;
        for (uint32_t j = 0; j < candidate->count; j++) {
            if (used[j]) continue;
            float sim = edge_similarity(txn, &query->items[i], &candidate->items[j]);
            if (sim > best) {
                best = sim;
                best_j = j;
            }
        }
        if (best_j >= 0) {
            used[best_j] = 1;
            result[matched].query = query->items[i].key;
            result[matched].candidate = candidate->items[best_j].key;
            result[matched].similarity = best;
            matched++;
        }
    }

    if (matched == 0) {
        free(result);
        result = NULL;
    } else {
        EdgeMatch *tmp = realloc(result, (size_t)matched * sizeof(EdgeMatch));
        if (tmp) result = tmp;
    }

    free(used);
    *matches = result;
    *count = matched;
    return MDB_SUCCESS;
}

// Сравнение двух рёбер
static float edge_similarity(MDB_txn *txn, const Edge *e1, const Edge *e2) {
    float source_sim = node_similarity(txn, e1->key.source, e2->key.source);
    float target_sim = node_similarity(txn, e1->key.target, e2->key.target);
    float rel_sim = relation_similarity(e1->key.relation, e2->key.relation);
    return 0.5f * rel_sim + 0.25f * source_sim + 0.25f * target_sim;
}

// F1-score для покрытия
static float coverage_score(int matched, int query, int candidate) {
    if (query <= 0 && candidate <= 0) return 1.f;
    if (query <= 0 || candidate <= 0) return 0.f;
    float precision = (float)matched / (float)candidate;
    float recall    = (float)matched / (float)query;
    if (precision + recall == 0.f) return 0.f;
    return 2.f * precision * recall / (precision + recall);
}

// Основная функция оценки аналогии
static AnalogyEvaluation analogy_fast_score(
    MDB_txn *txn,
    node_id_t query_node,
    node_id_t candidate,
    EdgeList *in_edges,
    EdgeList *out_edges,
    EdgeList *cand_in,
    EdgeList *cand_out,
    const ReasoningStrategy *strategy)
{
    (void)strategy;  // пока не используется

    AnalogyEvaluation eval = {0};
    EdgeMatch *in_matches = NULL, *out_matches = NULL;
    int in_count = 0, out_count = 0;

    if (match_edges(txn, in_edges, cand_in, &in_matches, &in_count) != MDB_SUCCESS)
        goto cleanup;
    if (match_edges(txn, out_edges, cand_out, &out_matches, &out_count) != MDB_SUCCESS)
        goto cleanup;

    float in_sum = 0.f;
    for (int i = 0; i < in_count; i++) in_sum += in_matches[i].similarity;

    float out_sum = 0.f;
    for (int i = 0; i < out_count; i++) out_sum += out_matches[i].similarity;

    eval.score.incoming = in_count ? in_sum / in_count : 0.f;
    eval.score.outgoing = out_count ? out_sum / out_count : 0.f;
    eval.score.center = node_similarity(txn, query_node, candidate);

    float in_cov = coverage_score(in_count, in_edges->count, cand_in->count);
    float out_cov = coverage_score(out_count, out_edges->count, cand_out->count);

    eval.score.coverage = (in_cov + out_cov) * 0.5f;

    // Корректировка, если покрытие слишком низкое
    float min_coverage = 0.3f;
    if (eval.score.coverage < min_coverage) {
        eval.score.total *= eval.score.coverage / min_coverage;
    }

    eval.score.neighborhood = 0.5f * eval.score.incoming + 0.5f * eval.score.outgoing;

    float rel_sum = 0.f;
    for (int i = 0; i < in_count; i++) {
        rel_sum += relation_similarity(in_matches[i].query.relation,
                                       in_matches[i].candidate.relation);
    }
    for (int i = 0; i < out_count; i++) {
        rel_sum += relation_similarity(out_matches[i].query.relation,
                                       out_matches[i].candidate.relation);
    }
    int total = in_count + out_count;
    eval.score.relation = total ? rel_sum / total : 0.f;

    eval.score.total =
        0.45f * eval.score.neighborhood +
        0.10f * eval.score.center +
        0.25f * eval.score.coverage +
        0.20f * eval.score.relation;

    // Лучшие совпадения
    if (in_count) {
        eval.best_incoming = in_matches[0];
        for (int i = 1; i < in_count; i++) {
            if (in_matches[i].similarity > eval.best_incoming.similarity)
                eval.best_incoming = in_matches[i];
        }
    }
    if (out_count) {
        eval.best_outgoing = out_matches[0];
        for (int i = 1; i < out_count; i++) {
            if (out_matches[i].similarity > eval.best_outgoing.similarity)
                eval.best_outgoing = out_matches[i];
        }
    }

cleanup:
    free(in_matches);
    free(out_matches);
    return eval;
}

// Поиск аналогичных паттернов для узла
int find_analogous_patterns(MDB_txn *txn, node_id_t query_node,
                            AnalogyCandidate **candidates, int *candidate_count) {
    int rc = MDB_SUCCESS;
    EdgeList in_edges = {0}, out_edges = {0};
    MDB_cursor *cursor = NULL;
    *candidates = NULL;
    *candidate_count = 0;

    rc = get_edges_to_node(txn, query_node, &in_edges);
    if (rc != MDB_SUCCESS) goto cleanup;

    rc = get_edges_from_node(txn, query_node, &out_edges);
    if (rc != MDB_SUCCESS) goto cleanup;

    rc = mdb_cursor_open(txn, db.graph.nodes, &cursor);
    if (rc != MDB_SUCCESS) goto cleanup;


    AnalogyCandidate temp[MAX_CANDIDATES_ANALOGY];
    int count = 0;
    MDB_val key, data;

    while ((rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT)) == MDB_SUCCESS) {
        node_id_t cand_id = *(node_id_t *)key.mv_data;
        if (cand_id == query_node) continue;

        EdgeList cand_in = {0}, cand_out = {0};

        rc = get_edges_to_node(txn, cand_id, &cand_in);
        if (rc != MDB_SUCCESS) goto cleanup_candidate;

        rc = get_edges_from_node(txn, cand_id, &cand_out);
        if (rc != MDB_SUCCESS) goto cleanup_candidate;

        // Исправленный вызов: передаём все 8 аргументов
        AnalogyEvaluation eval = analogy_fast_score(txn, query_node, cand_id,
                                                    &in_edges, &out_edges,
                                                    &cand_in, &cand_out,
                                                    NULL);

        if (eval.score.total >= 0.70f && count < MAX_CANDIDATES_ANALOGY) {
            AnalogyCandidate *c = &temp[count++];
            c->query_node = query_node;
            c->analogous_node = cand_id;
            c->query_condition = eval.best_incoming.query;
            c->analogous_condition = eval.best_incoming.candidate;
            c->query_result = eval.best_outgoing.query;
            c->analogous_result = eval.best_outgoing.candidate;
            c->score = eval.score;
        }

cleanup_candidate:
        free(cand_in.items);
        free(cand_out.items);
        if (rc != MDB_SUCCESS) goto cleanup;
        if (count >= MAX_CANDIDATES_ANALOGY) break;
    }

    if (rc == MDB_NOTFOUND) rc = MDB_SUCCESS;
    if (rc != MDB_SUCCESS) goto cleanup;

    if (count > 0) {
        *candidates = malloc(sizeof(AnalogyCandidate) * count);
        if (!*candidates) {
            rc = ENOMEM;
            goto cleanup;
        }
        memcpy(*candidates, temp, sizeof(AnalogyCandidate) * count);
        *candidate_count = count;
    }

cleanup:
    if (cursor) mdb_cursor_close(cursor);
    free(in_edges.items);
    free(out_edges.items);

    if (rc != MDB_SUCCESS) {
        free(*candidates);
        *candidates = NULL;
        *candidate_count = 0;
    }
    return rc;
}
