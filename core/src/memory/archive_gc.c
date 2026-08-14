// memory/archive_gc.c
//
// dbi_archive растёт неограниченно — subconscious_decay_cycle() только
// ПЕРЕМЕЩАЕТ атомы из dbi_atoms сюда, но никогда не удаляет их отсюда.
// Для потока сырых наблюдений (Задача 1) это и есть путь к "терабайтам
// мусора". hyper_memory_new_id() кодирует unix-время в старших 32 битах
// ((now<<32)|seq) — этого достаточно для приближённой оценки возраста без
// отдельного time-индекса. Round-robin по батчам — тот же паттерн, что
// subconscious_decay_cycle() уже использует для основной таблицы атомов.
#include <string.h>
#include <stdbool.h>
#include <time.h>

#include "storage/db/db.h"
#include "runtime/logging/logging.h"
#include "archive_gc.h"

#define ARCHIVE_GC_BATCH_SIZE 4096

static ko_id_t g_archive_resume_key = 0;
static bool    g_archive_resume_valid = false;

static inline uint32_t id_created_at(ko_id_t id) { return (uint32_t)(id >> 32); }

int archive_purge_cycle(MDB_txn *txn, HyperMemory *hmem, uint32_t max_age_sec, uint32_t *out_purged) {
    if (!txn || !hmem || !hmem->dbi_archive) return -1;

    uint32_t cutoff = (uint32_t)time(NULL) - max_age_sec;
    uint32_t purged = 0, scanned = 0;

    MDB_cursor *cursor;
    if (mdb_cursor_open(txn, hmem->dbi_archive, &cursor) != MDB_SUCCESS) return -1;

    MDB_val key, data;
    int rc;

    if (g_archive_resume_valid) {
        key.mv_size = sizeof(ko_id_t);
        key.mv_data = &g_archive_resume_key;
        rc = mdb_cursor_get(cursor, &key, &data, MDB_SET_RANGE);
        if (rc == MDB_NOTFOUND) rc = mdb_cursor_get(cursor, &key, &data, MDB_FIRST);
    } else {
        rc = mdb_cursor_get(cursor, &key, &data, MDB_FIRST);
    }

    while (rc == MDB_SUCCESS && scanned < ARCHIVE_GC_BATCH_SIZE) {
        ko_id_t id;
        memcpy(&id, key.mv_data, sizeof(ko_id_t));
        scanned++;

        if (id_created_at(id) < cutoff) {
            ko_id_t next_hint = id;
            MDB_val peek_key, peek_data;
            int peek_rc = mdb_cursor_get(cursor, &peek_key, &peek_data, MDB_NEXT);

            mdb_cursor_del(cursor, 0);
            purged++;

            if (peek_rc == MDB_SUCCESS) {
                memcpy(&g_archive_resume_key, peek_key.mv_data, sizeof(ko_id_t));
                g_archive_resume_valid = true;
                key.mv_size = sizeof(ko_id_t);
                key.mv_data = &g_archive_resume_key;
                rc = mdb_cursor_get(cursor, &key, &data, MDB_SET_RANGE);
            } else {
                g_archive_resume_key = next_hint;
                g_archive_resume_valid = false;
                rc = MDB_NOTFOUND;
            }
            continue;
        }
        rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT);
    }

    if (rc == MDB_SUCCESS) {
        memcpy(&g_archive_resume_key, key.mv_data, sizeof(ko_id_t));
        g_archive_resume_valid = true;
    } else {
        g_archive_resume_valid = false;
    }

    mdb_cursor_close(cursor);
    if (out_purged) *out_purged = purged;
    LOG_MEMORY("[ARCHIVE_GC] scanned=%u purged=%u cutoff_age_sec=%u", scanned, purged, max_age_sec);
    return 0;
}
