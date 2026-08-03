// knowledge/episode.c
#include <string.h>
#include <stdio.h>

#include "episode.h"
#include "storage/db/db.h"
#include "math/hash.h"
#include "runtime/logging/logging.h"
#include "ipc/ipc.h"

static const char *kEPISODE_RECORDED = "EPISODE_RECORDED";

int episode_record(HyperMemory *hmem, const Episode *ep) {
    if (!hmem || !hmem->txn || !ep || ep->id == 0) return -1;

    // 1. Полный блоб -> db.memory.episodes
    MDB_val key  = { sizeof(ko_id_t), (void *)&ep->id };
    MDB_val data = { sizeof(Episode), (void *)ep };
    int rc = mdb_put(hmem->txn, db.memory.episodes, &key, &data, 0);

    if (rc != MDB_SUCCESS) {
        LOG_ERROR("episode_record: mdb_put(episodes) failed: %s", mdb_strerror(rc));
        return -1;
    }

    // 2. Атом-указатель
    NeuroAtom pointer = {0};
    pointer.id          = ep->id;
    pointer.process_id  = proc_make(djb2_hash(kEPISODE_RECORDED), PROC_KIND_EVENT);
    pointer.args[0].raw = HYPER_MAKE_REF(ep->goal_id);
    pointer.args[1].raw = HYPER_MAKE_REF(ep->algorithm_id);
    pointer.truth_mean       = ep->outcome;
    pointer.truth_confidence = 1.0f;   // это наблюдение, не гипотеза
    pointer.sti = 0.3f;                // не должен доминировать в activation spread
    pointer.lti = 0.2f;                // но должен переживать несколько decay-циклов
    pointer.utility = ep->outcome;
    pointer.valence = 0.0f;
    pointer.context_or_time_link = ep->context_id;

    rc = hyper_assert(hmem, &pointer);
    if (rc != MDB_SUCCESS) {
        LOG_ERROR("episode_record: hyper_assert(pointer) failed: %s", mdb_strerror(rc));
        return -1;
    }

    LOG_MEMORY("[EPISODE] id=%lu goal=%lu algo=%lu status=%d outcome=%.2f result_atom=%lu",
               (unsigned long)ep->id, (unsigned long)ep->goal_id,
               (unsigned long)ep->algorithm_id, ep->vm_status, ep->outcome,
               (unsigned long)ep->result_atom_id);

    // 3. Броадкаст события в Python (Pub/Sub)
    char event_buf[512];
    snprintf(event_buf, sizeof(event_buf),
             "{\"episode_id\": %llu, \"goal_id\": %llu, \"algorithm_id\": %llu, \"outcome\": %.2f, \"status\": %d}",
             (unsigned long long)ep->id, (unsigned long long)ep->goal_id,
             (unsigned long long)ep->algorithm_id, ep->outcome, ep->vm_status);

    ipc_emit_event("EpisodeRecorded", event_buf);

    return 0;
}

int episode_load(MDB_txn *txn, ko_id_t episode_id, Episode *out) {
    if (!txn || !out || episode_id == 0) return -1;
    MDB_val key = { sizeof(ko_id_t), (void *)&episode_id };
    MDB_val data;

    int rc = mdb_get(txn, db.memory.episodes, &key, &data);
    if (rc != MDB_SUCCESS) return rc;

    if (data.mv_size != sizeof(Episode)) {
        LOG_ERROR("episode_load: size mismatch for id=%lu (got %zu, expected %zu)",
                   (unsigned long)episode_id, data.mv_size, sizeof(Episode));
        return MDB_BAD_VALSIZE;
    }
    memcpy(out, data.mv_data, sizeof(Episode));
    return MDB_SUCCESS;
}
