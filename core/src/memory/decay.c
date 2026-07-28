// memory/decay.c
#include <string.h>
#include <stdlib.h>

#include "decay.h"
#include "storage/db/db.h"
#include "runtime/logging/logging.h"

const DecayPolicy DECAY_POLICY_DEFAULT = {
    .sti_decay_factor          = 0.90f,
    .valence_regression        = 0.05f,
    .truth_conf_decay          = 0.995f,
    .sti_archive_threshold     = 0.05f,
    .lti_archive_threshold     = 0.05f,
    .utility_archive_threshold = 0.10f,
    .batch_size                = 2048
};

// Сохраняем позицию курсора между вызовами для round-robin сканирования
// всей таблицы небольшими порциями (bounded cycle).
static ko_id_t g_resume_key = 0;
static bool    g_resume_valid = false;

static void regress_to_zero(float *v, float rate) {
    *v = *v * (1.0f - rate);
    if (*v > -1e-4f && *v < 1e-4f) *v = 0.0f;
}

// Удаляет все вхождения атома из вторичных индексов перед архивацией.
static void remove_index_entries(MDB_txn *txn, HyperMemory *hmem, const NeuroAtom *atom) {
    MDB_cursor *cur;

    // idx_process: process_id -> id
    if (mdb_cursor_open(txn, hmem->dbi_idx_process, &cur) == MDB_SUCCESS) {
        MDB_val k = { sizeof(ko_id_t), (void *)&atom->process_id };
        MDB_val v = { sizeof(ko_id_t), (void *)&atom->id };
        if (mdb_cursor_get(cur, &k, &v, MDB_GET_BOTH) == MDB_SUCCESS)
            mdb_cursor_del(cur, 0);
        mdb_cursor_close(cur);
    }

    // idx_args: ref_arg -> id  (до 2 слотов теперь)
    if (mdb_cursor_open(txn, hmem->dbi_idx_args, &cur) == MDB_SUCCESS) {
        for (int i = 0; i < 2; i++) {
            if (HYPER_GET_TYPE(atom->args[i].raw) != HYPER_TYPE_REF) continue;
            ko_id_t ref = HYPER_GET_ID(atom->args[i].raw);
            MDB_val k = { sizeof(ko_id_t), (void *)&ref };
            MDB_val v = { sizeof(ko_id_t), (void *)&atom->id };
            if (mdb_cursor_get(cur, &k, &v, MDB_GET_BOTH) == MDB_SUCCESS)
                mdb_cursor_del(cur, 0);
        }
        mdb_cursor_close(cur);
    }

    // idx_context: context_or_time_link -> id
    if (mdb_cursor_open(txn, hmem->dbi_idx_context, &cur) == MDB_SUCCESS) {
        MDB_val k = { sizeof(ko_id_t), (void *)&atom->context_or_time_link };
        MDB_val v = { sizeof(ko_id_t), (void *)&atom->id };
        if (mdb_cursor_get(cur, &k, &v, MDB_GET_BOTH) == MDB_SUCCESS)
            mdb_cursor_del(cur, 0);
        mdb_cursor_close(cur);
    }

    // idx_causal: child_id -> cause_id  (атом как child — удаляем всю dup-группу)
    if (hmem->dbi_idx_causal && mdb_cursor_open(txn, hmem->dbi_idx_causal, &cur) == MDB_SUCCESS) {
        MDB_val k = { sizeof(ko_id_t), (void *)&atom->id };
        MDB_val v;
        if (mdb_cursor_get(cur, &k, &v, MDB_SET) == MDB_SUCCESS) {
            mdb_cursor_del(cur, MDB_NODUPDATA);
        }
        mdb_cursor_close(cur);
    }
}

static void archive_atom(MDB_txn *txn, HyperMemory *hmem, const NeuroAtom *atom) {
    if (!hmem->dbi_archive) return; // архив не сконфигурирован — просто пропускаем

    MDB_val k = { sizeof(ko_id_t), (void *)&atom->id };
    MDB_val v = { sizeof(NeuroAtom), (void *)atom };
    int rc = mdb_put(txn, hmem->dbi_archive, &k, &v, 0);
    if (rc != MDB_SUCCESS) {
        LOG_WARN("[DECAY] Failed to archive atom %lu: %s", (unsigned long)atom->id, mdb_strerror(rc));
        return;
    }

    remove_index_entries(txn, hmem, atom);
    mdb_del(txn, hmem->dbi_atoms, &k, NULL);
}

int subconscious_decay_cycle(HyperMemory *hmem, const DecayPolicy *policy, DecayStats *out_stats) {
    if (!hmem || !hmem->txn) return -1;
    if (!policy) policy = &DECAY_POLICY_DEFAULT;

    DecayStats stats = {0};

    MDB_cursor *cursor;
    int rc = mdb_cursor_open(hmem->txn, hmem->dbi_atoms, &cursor);
    if (rc != MDB_SUCCESS) {
        LOG_ERROR("[DECAY] mdb_cursor_open failed: %s", mdb_strerror(rc));
        return rc;
    }

    MDB_val key, data;

    // Возобновляем скан с прошлой позиции (round-robin по всей таблице).
    if (g_resume_valid) {
        key.mv_size = sizeof(ko_id_t);
        key.mv_data = &g_resume_key;
        rc = mdb_cursor_get(cursor, &key, &data, MDB_SET_RANGE);
        if (rc == MDB_NOTFOUND) {
            // Дошли до конца таблицы прошлый раз — начинаем сначала.
            rc = mdb_cursor_get(cursor, &key, &data, MDB_FIRST);
        }
    } else {
        rc = mdb_cursor_get(cursor, &key, &data, MDB_FIRST);
    }

    uint32_t processed = 0;
    while (rc == MDB_SUCCESS && processed < policy->batch_size) {
        if (data.mv_size != sizeof(NeuroAtom)) {
            rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT);
            continue;
        }

        NeuroAtom atom;
        memcpy(&atom, data.mv_data, sizeof(NeuroAtom));
        stats.scanned++;
        processed++;

        // --- Термический распад когнитивных векторов ---
        atom.sti *= policy->sti_decay_factor;
        if (atom.sti < 0.0f) atom.sti = 0.0f;

        regress_to_zero(&atom.valence, policy->valence_regression);

        atom.truth_confidence *= policy->truth_conf_decay;
        if (atom.truth_confidence < 0.0f) atom.truth_confidence = 0.0f;

        // LTI почти не угасает сам по себе — только сильно недоиспользуемые
        // атомы (низкий utility) постепенно теряют и его.
        if (atom.utility < policy->utility_archive_threshold) {
            atom.lti *= 0.999f;
        }

        bool is_cold =
            atom.sti < policy->sti_archive_threshold &&
            atom.lti < policy->lti_archive_threshold &&
            atom.utility < policy->utility_archive_threshold;

        if (is_cold) {
            // Архивируем и удаляем из горячей таблицы. Курсор инвалидируется
            // операцией mdb_del внутри archive_atom -> получаем следующий ключ
            // ДО удаления, чтобы не потерять позицию скана.
            ko_id_t next_key_hint = atom.id;
            MDB_val peek_key, peek_data;
            int peek_rc = mdb_cursor_get(cursor, &peek_key, &peek_data, MDB_NEXT);

            archive_atom(hmem->txn, hmem, &atom);
            stats.archived++;

            if (peek_rc == MDB_SUCCESS) {
                memcpy(&g_resume_key, peek_key.mv_data, sizeof(ko_id_t));
            } else {
                g_resume_key = next_key_hint; // конец таблицы — при следующем цикле уйдём на FIRST
                g_resume_valid = false;
                break;
            }
            g_resume_valid = true;

            // Курсор после mdb_del на LMDB остаётся валиден для след. позиции,
            // но безопаснее заново позиционироваться по g_resume_key.
            key.mv_size = sizeof(ko_id_t);
            key.mv_data = &g_resume_key;
            rc = mdb_cursor_get(cursor, &key, &data, MDB_SET_RANGE);
            continue;
        }

        // Не архивируем — просто перезаписываем decay'нутые значения на месте.
        MDB_val new_data = { sizeof(NeuroAtom), &atom };
        rc = mdb_cursor_put(cursor, &key, &new_data, MDB_CURRENT);
        if (rc != MDB_SUCCESS) {
            LOG_WARN("[DECAY] cursor_put failed for atom %lu: %s",
                     (unsigned long)atom.id, mdb_strerror(rc));
        } else {
            stats.updated++;
        }

        rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT);
    }

    if (rc == MDB_SUCCESS) {
        memcpy(&g_resume_key, key.mv_data, sizeof(ko_id_t));
        g_resume_valid = true;
    } else {
        // конец таблицы достигнут — следующий цикл начнётся с начала
        g_resume_valid = false;
    }

    mdb_cursor_close(cursor);

    if (out_stats) *out_stats = stats;

    LOG_MEMORY("[DECAY] cycle: scanned=%u updated=%u archived=%u",
               stats.scanned, stats.updated, stats.archived);

    return 0;
}
