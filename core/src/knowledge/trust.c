// knowledge/trust.c
#include <string.h>

#include "trust.h"
#include "math/hash.h"
#include "runtime/logging/logging.h"

// Детерминированный id rollup-атома доверия. Один и тот же (domain, subject_id)
// всегда даёт один и тот же id -> point lookup за O(1), без сканов и без
// генерации id по времени (в отличие от HAS_FLAW/CONFIDENCE_DELTA в
// critic_ops.c, которым время нужно, а нам — нет: это не evidence-лог,
// а единственная актуальная сводка).
static node_id_t trust_atom_id(TrustDomain domain, node_id_t subject_id) {
    uint64_t tag = 0x7275737421ULL ^ ((uint64_t)domain << 48); // "trust!" ^ domain
    return (subject_id ^ tag) & HYPER_VALUE_MASK;
}

// Ищет rollup-атом сначала в горячей таблице, затем в архиве. Повторное
// обращение к редко нужному знанию реактивирует его — см. docs/03_Knowledge.md:
// "Memory может повторно их активировать". Сегодня dbi_archive у
// global_hyper_mem ещё не подключён в main.c (см. RFC-0001, раздел
// "Наблюдения"), поэтому архивная ветка сейчас безопасно промахивается —
// это forward-compatible код, а не мёртвый.
static bool load_trust_atom(HyperMemory *hmem, node_id_t atom_id, NeuroAtom *out) {
    MDB_val key = { sizeof(atom_id), &atom_id };
    MDB_val data;

    if (mdb_get(hmem->txn, hmem->dbi_atoms, &key, &data) == MDB_SUCCESS &&
        data.mv_size == sizeof(NeuroAtom)) {
        memcpy(out, data.mv_data, sizeof(NeuroAtom));
        return true;
    }

    if (hmem->dbi_archive &&
        mdb_get(hmem->txn, hmem->dbi_archive, &key, &data) == MDB_SUCCESS &&
        data.mv_size == sizeof(NeuroAtom)) {
        memcpy(out, data.mv_data, sizeof(NeuroAtom));
        return true;
    }

    return false;
}

float trust_get(HyperMemory *hmem, TrustDomain domain, node_id_t subject_id) {
    if (!hmem || !hmem->txn) return TRUST_PRIOR;

    NeuroAtom atom;
    node_id_t id = trust_atom_id(domain, subject_id);

    if (!load_trust_atom(hmem, id, &atom))
        return TRUST_PRIOR;

    return atom.truth_mean;
}

int trust_update(HyperMemory *hmem, TrustDomain domain, node_id_t subject_id, bool success) {
    if (!hmem || !hmem->txn) return -1;

    NeuroAtom atom;
    node_id_t id = trust_atom_id(domain, subject_id);

    if (!load_trust_atom(hmem, id, &atom)) {
        memset(&atom, 0, sizeof(atom));
        atom.id            = id;
        atom.process_id    = proc_make(djb2_hash("TRUST_SCORE"), PROC_KIND_RELATION);
        atom.args[0].raw   = HYPER_MAKE_REF(subject_id);
        atom.args[1].raw   = (ko_id_t)domain | HYPER_TYPE_INT;
        atom.truth_mean       = TRUST_PRIOR;
        atom.truth_confidence = 0.0f;
    }

    // ВАЖНО: process_id/args не меняются между вызовами для одного и того же
    // id — иначе в idx_process/idx_args останутся осиротевшие записи.
    if (success) {
        atom.truth_mean += (1.0f - atom.truth_mean) * TRUST_LEARNING_RATE;
    } else {
        atom.truth_mean -= atom.truth_mean * TRUST_LEARNING_RATE;
    }
    atom.truth_confidence += (1.0f - atom.truth_confidence) * TRUST_LEARNING_RATE;

    // sti/lti/utility подключают эту запись к уже существующему циклу
    // decay/archive (memory/decay.c) — см. комментарий в trust.h.
    atom.sti     = 0.8f;
    atom.lti     = atom.truth_confidence;
    atom.utility = atom.truth_mean;

    atom.context_or_time_link = 0;

    int rc = hyper_assert(hmem, &atom);
    if (rc != MDB_SUCCESS) {
        LOG_ERROR("trust_update: hyper_assert failed for domain=%d subject=%lu: %s",
                   domain, (unsigned long)subject_id, mdb_strerror(rc));
    } else {
        LOG_PLANNER("[TRUST] domain=%d subject=%lu success=%d trust=%.3f conf=%.3f",
                    domain, (unsigned long)subject_id, success,
                    atom.truth_mean, atom.truth_confidence);
    }
    return rc;
}
