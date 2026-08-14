// knowledge/event_queue.c
#include <string.h>
#include "event_queue.h"
#include "runtime/logging/logging.h"

int event_queue_push(MDB_txn *txn, HyperMemory *hmem, ko_id_t queue_id, ko_id_t atom_id) {
    if (!txn || !hmem || !hmem->dbi_idx_pending) return -1;

    MDB_val k = { sizeof(ko_id_t), &queue_id };
    MDB_val v = { sizeof(ko_id_t), &atom_id };

    int rc = mdb_put(txn, hmem->dbi_idx_pending, &k, &v, 0);
    if (rc != MDB_SUCCESS) {
        LOG_ERROR("event_queue_push: queue=%lu atom=%lu: %s",
                  (unsigned long)queue_id, (unsigned long)atom_id, mdb_strerror(rc));
        return -1;
    }
    return 0;
}

int event_queue_pop_batch(MDB_txn *txn, HyperMemory *hmem, ko_id_t queue_id,
                           ko_id_t *out_atom_ids, uint32_t max_count) {
    if (!txn || !hmem || !hmem->dbi_idx_pending || !out_atom_ids) return -1;
    if (max_count > EVENT_QUEUE_MAX_POP) max_count = EVENT_QUEUE_MAX_POP;

    MDB_cursor *cur;
    if (mdb_cursor_open(txn, hmem->dbi_idx_pending, &cur) != MDB_SUCCESS) return -1;

    MDB_val k = { sizeof(ko_id_t), &queue_id };
    MDB_val v;
    uint32_t popped = 0;

    int rc = mdb_cursor_get(cur, &k, &v, MDB_SET);
    while (rc == MDB_SUCCESS && popped < max_count) {
        if (v.mv_size == sizeof(ko_id_t)) {
            memcpy(&out_atom_ids[popped], v.mv_data, sizeof(ko_id_t));
            popped++;
        }
        // Тот же приём, что hyper_ops.c::remap_causal_index уже использует:
        // del текущей дубль-записи, затем NEXT_DUP продолжает обход корректно.
        mdb_cursor_del(cur, 0);
        rc = mdb_cursor_get(cur, &k, &v, MDB_NEXT_DUP);
    }

    mdb_cursor_close(cur);
    return (int)popped;
}

uint32_t event_queue_len(MDB_txn *txn, HyperMemory *hmem, ko_id_t queue_id) {
    if (!txn || !hmem || !hmem->dbi_idx_pending) return 0;
    MDB_cursor *cur;
    if (mdb_cursor_open(txn, hmem->dbi_idx_pending, &cur) != MDB_SUCCESS) return 0;

    MDB_val k = { sizeof(ko_id_t), &queue_id };
    MDB_val v;
    uint32_t count = 0;
    int rc = mdb_cursor_get(cur, &k, &v, MDB_SET);
    while (rc == MDB_SUCCESS) { count++; rc = mdb_cursor_get(cur, &k, &v, MDB_NEXT_DUP); }
    mdb_cursor_close(cur);
    return count;
}
