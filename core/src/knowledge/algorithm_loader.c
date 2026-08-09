// core/src/knowledge/algorithm_loader.c
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
        // ФИКС: "не найдено" здесь — ШТАТНЫЙ, ожидаемый исход (например,
        // MainLoop/CorePlanner ещё не залиты в LMDB на старте ядра, до
        // выполнения bootstrap.py — субсознание опрашивает это каждую
        // итерацию демона). Это не архитектурная ошибка, поэтому не
        // должно засорять system.log на уровне WARN — при активном
        // старте это давало десятки строк в секунду (см. приложенные
        // логи ядра). Подлинные "not found", важные для отладки,
        // по-прежнему видны в debug.log.
        LOG_DEBUG("Algorithm %lu not found in DB", algo_id);
        return rc;
    }

    if (data.mv_size < sizeof(uint32_t) * 2) {
        LOG_WARN("Algorithm %lu data too small", algo_id);
        return -1;
    }

    const uint8_t *raw = (const uint8_t *)data.mv_data;
    uint32_t code_len, capacity;
    memcpy(&code_len, raw, sizeof(uint32_t));
    memcpy(&capacity, raw + sizeof(uint32_t), sizeof(uint32_t));
    const Instruction *code_start = (const Instruction *)(raw + 2 * sizeof(uint32_t));
    size_t code_bytes = code_len * sizeof(Instruction);

    if (capacity < code_len)
        return -1;

    if (capacity > MAX_PIPELINE_CODE)
        return -1;

    if (sizeof(uint32_t) * 2 + code_bytes > data.mv_size) {
        LOG_WARN("Algorithm %lu data truncated", algo_id);
        return -1;
    }

    Pipeline *p = pipeline_create_with_capacity(capacity);
    if (!p) return -1;

    memcpy(p->code, code_start, code_bytes);
    p->code_len = code_len;

    /* Восстанавливаем константы */
    const uint8_t *cptr = raw + sizeof(uint32_t) * 2 + code_bytes;
    const uint8_t *end  = raw + data.mv_size;

    // ints
    if (cptr + sizeof(uint32_t) > end) goto done;
    uint32_t n;
    memcpy(&n, cptr, sizeof(uint32_t)); cptr += sizeof(uint32_t);
    p->constants.int_count = n;
    if (n > 0) {
        if (cptr + n * sizeof(int64_t) > end) goto cleanup;
        p->constants.int_consts = malloc(n * sizeof(int64_t));
        if (!p->constants.int_consts) goto cleanup;
        memcpy(p->constants.int_consts, cptr, n * sizeof(int64_t));
        cptr += n * sizeof(int64_t);
    }

    // floats
    if (cptr + sizeof(uint32_t) > end) goto done;
    memcpy(&n, cptr, sizeof(uint32_t)); cptr += sizeof(uint32_t);
    p->constants.float_count = n;
    if (n > 0) {
        if (cptr + n * sizeof(double) > end) goto cleanup;
        p->constants.float_consts = malloc(n * sizeof(double));
        if (!p->constants.float_consts) goto cleanup;
        memcpy(p->constants.float_consts, cptr, n * sizeof(double));
        cptr += n * sizeof(double);
    }

    // strings
    if (cptr + sizeof(uint32_t) > end) goto done;
    memcpy(&n, cptr, sizeof(uint32_t)); cptr += sizeof(uint32_t);
    p->constants.str_count = n;
    if (n > 0) {
        p->constants.str_consts = malloc(n * sizeof(StringView));
        if (!p->constants.str_consts) goto cleanup;
        for (uint32_t i = 0; i < n; i++) {
            if (cptr + sizeof(uint32_t) > end) goto cleanup;
            uint32_t slen;
            memcpy(&slen, cptr, sizeof(uint32_t)); cptr += sizeof(uint32_t);
            if (cptr + slen > end) goto cleanup;
            char *s = malloc(slen + 1);
            if (!s) goto cleanup;
            memcpy(s, cptr, slen);
            s[slen] = '\0';
            p->constants.str_consts[i].data = s;
            p->constants.str_consts[i].len  = slen;
            cptr += slen;
        }
    }
    /* Pipeline I/O signature.
     * Old records may not contain this section; keep legacy defaults.
     */
    if (cptr + sizeof(uint32_t) <= end) {
        memcpy(&n, cptr, sizeof(uint32_t));
        cptr += sizeof(uint32_t);

        if (n > 8)
            goto cleanup;

        p->in_count = (uint8_t)n;

        if (n > 0) {
            if (cptr + n * sizeof(uint8_t) > end)
                goto cleanup;

            memcpy(p->in_regs, cptr, n * sizeof(uint8_t));
            cptr += n * sizeof(uint8_t);
        }
    }

    if (cptr + sizeof(uint32_t) <= end) {
        memcpy(&n, cptr, sizeof(uint32_t));
        cptr += sizeof(uint32_t);

        if (n > 8)
            goto cleanup;

        p->out_count = (uint8_t)n;

        if (n > 0) {
            if (cptr + n * sizeof(uint8_t) > end)
                goto cleanup;

            memcpy(p->out_regs, cptr, n * sizeof(uint8_t));
            cptr += n * sizeof(uint8_t);
        }
    }

done:
    *out_pipeline = p;
    return 0;

cleanup:
    pipeline_free(p);

    return -1;
}
