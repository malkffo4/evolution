// ipc/handlers/request/req_advise.c
//
// "Safety Gate" для режима пассивного аналитика. В отличие от req_retrieve
// (сырой дамп графа), этот хендлер:
//   1) отбрасывает атомы, уже помеченные Critic'ом HAS_FLAW(atom, GarbageCandidate)
//      (critic_ops.c::vm_op_critic_apply, knowledge_validator.py::mark_flaw);
//   2) отбрасывает всё ниже ADVISE_MIN_CONFIDENCE — не советуем на шуме;
//   3) НИКОГДА не возвращает атом без явной метки уровня доверия
//      (verified/probable/hypothesis) — вызывающий код физически не может
//      случайно процитировать гипотезу как факт, потому что тег обязателен
//      в самой структуре ответа.
//
// Транзакция: MDB_RDONLY, открывается и закрывается прямо в IPC-потоке —
// тот же паттерн, что req_retrieve.c/req_find_similar.c. db_writer не
// участвует: чистое чтение, никакой мутации.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <cjson/cJSON.h>

#include "ipc/ipc.h"
#include "storage/db/db.h"
#include "storage/hyper_atom/hyper_atom.h"
#include "storage/string_pool/string_pool.h"
#include "math/hash.h"

#define ADVISE_MAX_RESULTS    30
#define ADVISE_MIN_CONFIDENCE 0.15f

typedef struct {
    NeuroAtom atom;
    float rank;
} AdviseCandidate;

static bool has_flaw(MDB_txn *txn, HyperMemory *hmem, node_id_t atom_id) {
    static node_id_t flaw_proc = 0;
    if (!flaw_proc) flaw_proc = proc_make(djb2_hash("HAS_FLAW"), PROC_KIND_RELATION);

    NeuroAtom *related = NULL;
    size_t count = 0;
    bool flawed = false;

    // O(fan-out атома), не скан базы — тот же приём, что composition_ops.c
    // использует для локального поиска связей.
    if (hyper_find_by_participant(txn, hmem, atom_id, 0, &related, &count) == 0) {
        for (size_t i = 0; i < count; i++) {
            if (related[i].process_id == flaw_proc &&
                HYPER_GET_ID(related[i].args[0].raw) == atom_id) {
                flawed = true;
                break;
            }
        }
    }
    if (related) free(related);
    return flawed;
}

static const char *confidence_tier(float conf) {
    if (conf >= 0.75f) return "verified";
    if (conf >= 0.50f) return "probable";
    return "hypothesis";
}

static int cmp_candidates(const void *a, const void *b) {
    const AdviseCandidate *ca = a, *cb = b;
    if (ca->rank > cb->rank) return -1;
    if (ca->rank < cb->rank) return 1;
    return 0;
}

void req_advise(IPCPacket *req, IPCPacket *resp) {
    cJSON *json = cJSON_Parse((const char *)req->payload);
    char query[256] = {0};
    if (json) {
        cJSON *q = cJSON_GetObjectItemCaseSensitive(json, "query");
        if (cJSON_IsString(q) && q->valuestring)
            strncpy(query, q->valuestring, sizeof(query) - 1);
        cJSON_Delete(json);
    }

    resp->type = IPC_RESPONSE;
    strncpy(resp->name, "advise", sizeof(resp->name) - 1);

    if (query[0] == '\0') {
        const char *err = "{\"error\": \"query required\"}";
        strncpy(resp->payload, err, sizeof(resp->payload) - 1);
        resp->payload_size = (uint32_t)strlen(err);
        return;
    }

    node_id_t participant_id = djb2_hash(query);

    MDB_txn *txn;
    if (mdb_txn_begin(db.env, NULL, MDB_RDONLY, &txn) != MDB_SUCCESS) {
        const char *err = "{\"error\": \"DB transaction failed\"}";
        strncpy(resp->payload, err, sizeof(resp->payload) - 1);
        resp->payload_size = (uint32_t)strlen(err);
        return;
    }

    HyperMemory hmem = {0};
    hmem.dbi_atoms      = db.graph.hyper.atoms;
    hmem.dbi_idx_process = db.graph.hyper.idx_process;
    hmem.dbi_idx_args    = db.graph.hyper.idx_args;
    hmem.dbi_idx_context = db.graph.hyper.idx_context;

    NeuroAtom *found = NULL;
    size_t count = 0;
    cJSON *arr = cJSON_CreateArray();

    if (hyper_find_by_participant(txn, &hmem, participant_id, 0, &found, &count) == 0 && found) {
        AdviseCandidate *cands = calloc(count, sizeof(AdviseCandidate));
        size_t n = 0;

        if (cands) {
            static node_id_t episode_proc = 0;
            if (!episode_proc) episode_proc = proc_make(djb2_hash("EPISODE_RECORDED"), PROC_KIND_EVENT);

            for (size_t i = 0; i < count; i++) {
                if (found[i].process_id == episode_proc) continue;          // телеметрия исполнения — не знание
                if (found[i].truth_confidence < ADVISE_MIN_CONFIDENCE) continue;
                if (has_flaw(txn, &hmem, found[i].id)) continue;            // Critic уже отбраковал

                cands[n].atom = found[i];
                cands[n].rank = 0.4f * found[i].sti + 0.6f * found[i].truth_confidence;
                n++;
            }

            qsort(cands, n, sizeof(AdviseCandidate), cmp_candidates);
            size_t emit = n < ADVISE_MAX_RESULTS ? n : ADVISE_MAX_RESULTS;

            for (size_t i = 0; i < emit; i++) {
                NeuroAtom *a = &cands[i].atom;
                cJSON *item = cJSON_CreateObject();

                const char *proc_label = get_string_from_pool(txn, a->process_id & PROC_ID_MASK);
                cJSON_AddStringToObject(item, "process", proc_label ? proc_label : "UNKNOWN");
                cJSON_AddStringToObject(item, "confidence_tier", confidence_tier(a->truth_confidence));
                cJSON_AddNumberToObject(item, "confidence", a->truth_confidence);

                cJSON *args_arr = cJSON_AddArrayToObject(item, "args");
                for (int s = 0; s < HYPER_VAL_SLOTS; s++) {
                    if (a->args[s].raw == 0) continue;
                    char numbuf[32];
                    const char *label = (HYPER_GET_TYPE(a->args[s].raw) == HYPER_TYPE_REF)
                        ? get_string_from_pool(txn, HYPER_GET_ID(a->args[s].raw)) : NULL;
                    if (!label) {
                        snprintf(numbuf, sizeof(numbuf), "%lld", (long long)HYPER_GET_ID(a->args[s].raw));
                        label = numbuf;
                    }
                    cJSON_AddItemToArray(args_arr, cJSON_CreateString(label));
                }
                cJSON_AddItemToArray(arr, item);
            }
            free(cands);
        }
    }
    if (found) free(found);
    mdb_txn_abort(txn);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "query", query);
    cJSON_AddItemToObject(root, "advisories", arr);
    cJSON_AddStringToObject(root, "disclaimer",
        "Passive knowledge-graph analysis. 'hypothesis' entries are unconfirmed "
        "leads requiring human validation, never a basis for automated action.");

    char *s = cJSON_PrintUnformatted(root);
    snprintf(resp->payload, sizeof(resp->payload), "%s", s);
    resp->payload_size = (uint32_t)strlen(resp->payload);
    free(s);
    cJSON_Delete(root);
}
