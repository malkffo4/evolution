// reasoning/strategy_store.c
#include <string.h>
#include <math.h>

#include "reasoning/strategy_store.h"
#include "storage/db/db.h"
#include "storage/property.h"
#include "math/hash.h"

#define STRAT_NODE_ID djb2_hash("ReasoningStrategy:Analogy")

typedef struct { node_id_t nid; uint64_t hash; } PropKey;

static float prop_get_float(MDB_txn *txn, uint64_t key_hash, float def) {
    PropKey pk = { STRAT_NODE_ID, key_hash };
    MDB_val key = { sizeof(pk), &pk };
    MDB_val data;
    if (mdb_get(txn, db.graph.properties, &key, &data) != MDB_SUCCESS) return def;
    if (data.mv_size < sizeof(NodeProperty)) return def;
    NodeProperty hdr;
    memcpy(&hdr, data.mv_data, sizeof(hdr));
    if (hdr.type != PROP_FLOAT || data.mv_size < sizeof(hdr) + sizeof(float)) return def;
    float v;
    memcpy(&v, (const char *)data.mv_data + sizeof(hdr), sizeof(float));
    return v;
}

static void prop_set_float(MDB_txn *txn, uint64_t key_hash, float value) {
    PropKey pk = { STRAT_NODE_ID, key_hash };
    MDB_val key = { sizeof(pk), &pk };
    NodeProperty hdr = { .type = PROP_FLOAT, .size = sizeof(float) };
    char buf[sizeof(NodeProperty) + sizeof(float)];
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), &value, sizeof(float));
    MDB_val data = { sizeof(buf), buf };
    mdb_put(txn, db.graph.properties, &key, &data, 0);
}

void reasoning_weights_load(MDB_txn *txn, ReasoningWeights *out, uint32_t *out_step) {
    out->neighborhood = prop_get_float(txn, djb2_hash("w_neighborhood"), 0.45f);
    out->center        = prop_get_float(txn, djb2_hash("w_center"),       0.10f);
    out->coverage       = prop_get_float(txn, djb2_hash("w_coverage"),     0.25f);
    out->relation        = prop_get_float(txn, djb2_hash("w_relation"),     0.20f);
    if (out_step)
        *out_step = (uint32_t)prop_get_float(txn, djb2_hash("w_step_count"), 0.0f);
}

void reasoning_weights_sgd_update(MDB_txn *txn, const float x[4], float y) {
    ReasoningWeights w;
    uint32_t step = 0;
    reasoning_weights_load(txn, &w, &step);

    float wv[4] = { w.neighborhood, w.center, w.coverage, w.relation };
    float y_hat = 0.0f;
    for (int i = 0; i < 4; i++) y_hat += wv[i] * x[i];

    float eta = 0.10f / sqrtf(1.0f + (float)step);   // Robbins-Monro decay
    float err = y - y_hat;

    float sum = 0.0f;
    for (int i = 0; i < 4; i++) {
        wv[i] += eta * err * x[i];
        if (wv[i] < 0.0f) wv[i] = 0.0f;   // веса эвристик не могут быть отрицательными
        sum += wv[i];
    }
    if (sum < 1e-6f) sum = 1e-6f;
    for (int i = 0; i < 4; i++) wv[i] /= sum;   // проекция на симплекс (сумма = 1)

    prop_set_float(txn, djb2_hash("w_neighborhood"), wv[0]);
    prop_set_float(txn, djb2_hash("w_center"),       wv[1]);
    prop_set_float(txn, djb2_hash("w_coverage"),     wv[2]);
    prop_set_float(txn, djb2_hash("w_relation"),     wv[3]);
    prop_set_float(txn, djb2_hash("w_step_count"),   (float)(step + 1));
}