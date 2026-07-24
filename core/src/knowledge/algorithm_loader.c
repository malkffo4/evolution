// knowledge/algorithm_loader.c
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>

#include "algorithm_loader.h"
#include "runtime/logging/logging.h"
#include "storage/db/db.h"

int algorithm_load(MDB_txn *txn, node_id_t algo_id, Pipeline **out_pipeline) {
    if (!out_pipeline) return -1;

    MDB_val key, data;
    key.mv_size = sizeof(node_id_t);
    key.mv_data = &algo_id;

    int rc = mdb_get(txn, db.graph.algorithms, &key, &data);
    if (rc != MDB_SUCCESS) {
        LOG_WARN("Algorithm %lu not found in DB", algo_id);
        return rc;
    }

    if (data.mv_size < sizeof(uint32_t) * 2) {
        LOG_WARN("Algorithm %lu data too small", algo_id);
        return -1;
    }

    const uint8_t *raw = (const uint8_t *)data.mv_data;
    const uint32_t *header = (const uint32_t *)raw;
    uint32_t code_len = header[0];
    // uint32_t capacity = header[1];
    const Instruction *code_start = (const Instruction *)(raw + sizeof(uint32_t) * 2);
    size_t code_bytes = code_len * sizeof(Instruction);

    if (sizeof(uint32_t) * 2 + code_bytes > data.mv_size) {
        LOG_WARN("Algorithm %lu data truncated", algo_id);
        return -1;
    }

    Pipeline *p = pipeline_create();
    if (!p) return -1;

    memcpy(p->code, code_start, code_bytes);
    p->code_len = code_len;
    p->capacity = code_len;

    /* Восстанавливаем константы */
    const uint8_t *cptr = raw + sizeof(uint32_t) * 2 + code_bytes;
    const uint8_t *end  = raw + data.mv_size;

    // ints
    if (cptr + sizeof(uint32_t) > end) goto done;
    uint32_t n = *(uint32_t*)cptr; cptr += sizeof(uint32_t);
    p->constants.int_count = n;
    if (n > 0) {
        if (cptr + n * sizeof(int64_t) > end) goto cleanup;
        p->constants.int_consts = malloc(n * sizeof(int64_t));
        memcpy(p->constants.int_consts, cptr, n * sizeof(int64_t));
        cptr += n * sizeof(int64_t);
    }

    // floats
    if (cptr + sizeof(uint32_t) > end) goto done;
    n = *(uint32_t*)cptr; cptr += sizeof(uint32_t);
    p->constants.float_count = n;
    if (n > 0) {
        if (cptr + n * sizeof(double) > end) goto cleanup;
        p->constants.float_consts = malloc(n * sizeof(double));
        memcpy(p->constants.float_consts, cptr, n * sizeof(double));
        cptr += n * sizeof(double);
    }

    // strings
    if (cptr + sizeof(uint32_t) > end) goto done;
    n = *(uint32_t*)cptr; cptr += sizeof(uint32_t);
    p->constants.str_count = n;
    if (n > 0) {
        p->constants.str_consts = malloc(n * sizeof(StringView));
        for (uint32_t i = 0; i < n; i++) {
            if (cptr + sizeof(uint32_t) > end) goto cleanup;
            uint32_t slen = *(uint32_t*)cptr; cptr += sizeof(uint32_t);
            if (cptr + slen > end) goto cleanup;
            char *s = malloc(slen + 1);
            memcpy(s, cptr, slen);
            s[slen] = '\0';
            p->constants.str_consts[i].data = s;
            p->constants.str_consts[i].len  = slen;
            cptr += slen;
        }
    }

done:
    *out_pipeline = p;
    return 0;

cleanup:
    // частично освободить и вернуть ошибку
    pipeline_free(p);

    return -1;
}
