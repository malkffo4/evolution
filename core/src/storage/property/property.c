// core/src/storage/property.c
#include <string.h>
#include <stdlib.h>

#include "storage/db/db.h"
#include "property.h"
#include "math/hash.h"
#include "runtime/logging/logging.h"

typedef struct { node_id_t nid; uint64_t hash; } PropKey;

int property_set(MDB_txn *txn, node_id_t node_id, const char *key,
                  PropertyType type, const void *payload, uint32_t size) {
    if (!txn || !key) return -1;

    PropKey pk = { node_id, djb2_hash(key) };
    MDB_val mkey = { sizeof(pk), &pk };

    NodeProperty hdr = { .node_id = node_id, .key_hash = pk.hash, .type = type, .size = size };
    size_t total = sizeof(hdr) + size;

    uint8_t stackbuf[512];
    uint8_t *buf = (total <= sizeof(stackbuf)) ? stackbuf : malloc(total);
    if (!buf) return -1;

    memcpy(buf, &hdr, sizeof(hdr));
    if (size > 0 && payload) memcpy(buf + sizeof(hdr), payload, size);

    MDB_val mval = { total, buf };
    int rc = mdb_put(txn, db.graph.properties, &mkey, &mval, 0);

    if (buf != stackbuf) free(buf);
    if (rc != MDB_SUCCESS)
        LOG_ERROR("property_set failed node=%lu key=%s: %s",
                  (unsigned long)node_id, key, mdb_strerror(rc));
    return rc;
}

int property_get(MDB_txn *txn, node_id_t node_id, const char *key,
                  PropertyType *out_type, void *out_buf, uint32_t buf_size, uint32_t *out_size) {
    if (!txn || !key) return -1;

    PropKey pk = { node_id, djb2_hash(key) };
    MDB_val mkey = { sizeof(pk), &pk };
    MDB_val mval;

    int rc = mdb_get(txn, db.graph.properties, &mkey, &mval);
    if (rc != MDB_SUCCESS) return rc;
    if (mval.mv_size < sizeof(NodeProperty)) return MDB_BAD_VALSIZE;

    NodeProperty hdr;
    memcpy(&hdr, mval.mv_data, sizeof(hdr));
    if (mval.mv_size < sizeof(hdr) + hdr.size) return MDB_BAD_VALSIZE;

    if (out_type) *out_type = hdr.type;
    if (out_size) *out_size = hdr.size;
    if (out_buf) {
        if (buf_size < hdr.size) return MDB_BAD_VALSIZE;  // не отдаём частично-заполненный буфер
        memcpy(out_buf, (const uint8_t *)mval.mv_data + sizeof(hdr), hdr.size);
    }
    return MDB_SUCCESS;
}
