// storage/hyper_atom/hyper_pattern.c
#include <string.h>
#include "storage/hyper_atom/hyper_pattern.h"
#include "runtime/logging/logging.h"

int hyper_pattern_save(MDB_txn *txn, MDB_dbi dbi, const HyperPattern *pattern) {
    if (!txn || !pattern) return -1;
    MDB_val key  = { sizeof(ko_id_t), (void *)&pattern->id };
    MDB_val data = { sizeof(HyperPattern), (void *)pattern };
    int rc = mdb_put(txn, dbi, &key, &data, 0);
    if (rc != MDB_SUCCESS)
        LOG_ERROR("hyper_pattern_save failed: %s", mdb_strerror(rc));
    return rc;
}

int hyper_pattern_load(MDB_txn *txn, MDB_dbi dbi, ko_id_t pattern_id, HyperPattern *out) {
    if (!txn || !out) return -1;
    MDB_val key = { sizeof(ko_id_t), (void *)&pattern_id };
    MDB_val data;
    int rc = mdb_get(txn, dbi, &key, &data);
    if (rc != MDB_SUCCESS) return rc;
    if (data.mv_size != sizeof(HyperPattern)) return MDB_BAD_VALSIZE;
    memcpy(out, data.mv_data, sizeof(HyperPattern));
    return MDB_SUCCESS;
}
