// knowledge/evaluation.c
// Полная реализация RFC-0001 Adaptive Planner v2
// Идентичность — через process + args, поиск — через hyper_find_by_participant.
// Оценка разделена на неизменяемые Evaluation (наблюдения) и мутируемый Score (свёртка).

#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "evaluation.h"
#include "math/hash.h"
#include "runtime/logging/logging.h"
#include "storage/hyper_atom/hyper_atom.h"

// ----- Внутренние константы (имена процессов) ------------------------------
static const char *kOBSERVED_OUTCOME = "OBSERVED_OUTCOME";
static const char *kHAS_SCORE        = "HAS_SCORE";

// ----- Вспомогательные функции поиска Score --------------------------------

/*
 * Ищет существующий Score‑атом для (domain, subject_id).
 * Просматривает все атомы, где subject_id является участником (args[0]),
 * фильтрует по process_id == HAS_SCORE и совпадению домена в args[1].
 * Возвращает указатель на статически аллоцированный NeuroAtom (копию),
 * если найден, иначе NULL.
 * Вызывающая сторона НЕ владеет памятью.
 */
static NeuroAtom *find_score_atom(HyperMemory *hmem, CognitiveDomain domain, node_id_t subject_id) {
    NeuroAtom *atoms = NULL;
    size_t count = 0;

    // Ищем все атомы, где subject_id является участником (в любом слоте).
    if (hyper_find_by_participant(hmem, subject_id, 0, &atoms, &count) != 0 || !atoms)
        return NULL;

    NeuroAtom *found = NULL;
    node_id_t has_score_full = proc_make(djb2_hash("HAS_SCORE"), PROC_KIND_RELATION);

    for (size_t i = 0; i < count; i++) {
        NeuroAtom *a = &atoms[i];
        if (a->process_id != has_score_full) continue;
        // Проверяем, что args[0] ссылается на subject_id
        if (HYPER_GET_TYPE(a->args[0].raw) != HYPER_TYPE_REF ||
            HYPER_GET_ID(a->args[0].raw) != subject_id) continue;
        // Проверяем, что args[1] содержит нужный домен
        if (HYPER_GET_TYPE(a->args[1].raw) != HYPER_TYPE_INT ||
            (CognitiveDomain)(uint64_t)HYPER_GET_ID(a->args[1].raw) != domain) continue;

        // Нашли — возвращаем копию, чтобы не зависеть от времени жизни atoms.
        static NeuroAtom cached;
        memcpy(&cached, a, sizeof(NeuroAtom));
        found = &cached;
        break;
    }

    free(atoms);
    return found;
}

// ----- Создание или обновление Score «на месте» ---------------------------
/*
 * Сохраняет переданный Score‑атом в LMDB через hyper_assert (НЕ _unique),
 * что позволяет перезаписывать существующий атом с тем же id.
 * При первом создании id должен быть уже присвоен (вызывающая сторона
 * генерирует его через hyper_memory_new_id()).
 */
static int save_score(HyperMemory *hmem, NeuroAtom *score_atom) {
    // hyper_assert делает mdb_put с ключом = id, перезаписывая значение.
    // Это не нарушает индексы, потому что process_id и args не меняются
    // между вызовами для одного и того же субъекта+домена.
    int rc = hyper_assert(hmem, score_atom);
    if (rc != MDB_SUCCESS) {
        LOG_ERROR("save_score: hyper_assert failed: %s", mdb_strerror(rc));
        return -1;
    }
    return 0;
}

// ----- Публичный API -------------------------------------------------------

node_id_t evaluation_record(HyperMemory *hmem, CognitiveDomain domain,
                             node_id_t subject_id, float outcome,
                             node_id_t cause_id, node_id_t context_id) {
    if (!hmem || !hmem->txn) return 0;

    // Генерируем уникальный id для нового атома наблюдения.
    node_id_t eval_id = hyper_memory_new_id(hmem);
    NeuroAtom eval_atom;
    memset(&eval_atom, 0, sizeof(eval_atom));
    eval_atom.id               = eval_id;
    eval_atom.process_id        = proc_make(djb2_hash(kOBSERVED_OUTCOME), PROC_KIND_EVENT);
    eval_atom.args[0].raw      = HYPER_MAKE_REF(subject_id);
    eval_atom.args[1].raw      = (ko_id_t)(int64_t)domain | HYPER_TYPE_INT;
    eval_atom.truth_mean       = outcome;
    eval_atom.truth_confidence = 1.0f;   // наблюдение — достоверный факт
    eval_atom.sti              = 0.2f;
    eval_atom.lti              = 0.1f;
    eval_atom.utility          = 0.0f;
    eval_atom.valence          = 0.0f;
    eval_atom.context_or_time_link = context_id;

    // Пишем атом наблюдения (без проверки уникальности — каждое наблюдение уникально)
    int rc = hyper_assert(hmem, &eval_atom);
    if (rc != MDB_SUCCESS) {
        LOG_ERROR("evaluation_record: hyper_assert failed: %s", mdb_strerror(rc));
        return 0;
    }

    // При наличии причинности записываем связь child -> cause в idx_causal
    if (cause_id != 0 && hmem->dbi_idx_causal) {
        MDB_val k = { sizeof(node_id_t), &eval_id };
        MDB_val v = { sizeof(node_id_t), &cause_id };
        mdb_put(hmem->txn, hmem->dbi_idx_causal, &k, &v, MDB_APPENDDUP);
    }

    LOG_PLANNER("[EVAL] Recorded observation domain=%d subject=%lu outcome=%.3f (id=%lu)",
                domain, (unsigned long)subject_id, outcome, (unsigned long)eval_id);
    return eval_id;
}

float score_get(HyperMemory *hmem, CognitiveDomain domain, node_id_t subject_id) {
    if (!hmem || !hmem->txn) return SCORE_PRIOR;

    NeuroAtom *score = find_score_atom(hmem, domain, subject_id);
    if (!score) return SCORE_PRIOR;

    return score->truth_mean;
}

int score_update(HyperMemory *hmem, CognitiveDomain domain, node_id_t subject_id,
                  float outcome, node_id_t cause_id, node_id_t context_id) {
    return score_update_weighted(hmem, domain, subject_id, outcome, 1.0f, cause_id, context_id);
}

int score_update_weighted(HyperMemory *hmem, CognitiveDomain domain, node_id_t subject_id,
                           float outcome, float credit_weight,
                           node_id_t cause_id, node_id_t context_id) {
    if (!hmem || !hmem->txn) return -1;
    if (credit_weight > 1.0f) credit_weight = 1.0f;
    if (credit_weight <= 0.0f) return 0; // нулевой вклад — не засоряем историю

    // 1. Неизменяемое наблюдение. Пишем полный outcome без затухания —
    //    вес влияет только на скорость обучения Score, а не на факт.
    if (evaluation_record(hmem, domain, subject_id, outcome, cause_id, context_id) == 0)
        return -1;

    // 2. Найти/создать Score
    NeuroAtom *existing = find_score_atom(hmem, domain, subject_id);
    NeuroAtom score_atom;
    bool is_new = (existing == NULL);
    if (is_new) {
        memset(&score_atom, 0, sizeof(score_atom));
        score_atom.id           = hyper_memory_new_id(hmem);
        score_atom.process_id   = proc_make(djb2_hash(kHAS_SCORE), PROC_KIND_RELATION);
        score_atom.args[0].raw  = HYPER_MAKE_REF(subject_id);
        score_atom.args[1].raw  = (ko_id_t)(int64_t)domain | HYPER_TYPE_INT;
        score_atom.truth_mean       = SCORE_PRIOR;
        score_atom.truth_confidence = 0.0f;
    } else {
        memcpy(&score_atom, existing, sizeof(NeuroAtom));
    }

    // 3. Канонический EMA к outcome, ослабленный credit_weight.
    //    credit_weight=1.0 побитово эквивалентен прежней бинарной формуле
    //    для outcome ∈ {0.0, 1.0} (все текущие вызовы именно такие).
    float effective_lr = SCORE_LEARNING_RATE * credit_weight;
    score_atom.truth_mean       += (outcome - score_atom.truth_mean) * effective_lr;
    score_atom.truth_confidence += (1.0f - score_atom.truth_confidence) * effective_lr;

    score_atom.sti     = 0.8f * credit_weight; // косвенная причина — меньше внимания
    score_atom.lti     = score_atom.truth_confidence;
    score_atom.utility = score_atom.truth_mean;
    score_atom.valence = 0.0f;
    score_atom.context_or_time_link = 0;

    int rc = save_score(hmem, &score_atom);
    if (rc == 0) {
        LOG_PLANNER("[SCORE] weighted domain=%d subject=%lu outcome=%.3f w=%.3f -> trust=%.3f conf=%.3f",
                    domain, (unsigned long)subject_id, outcome, credit_weight,
                    score_atom.truth_mean, score_atom.truth_confidence);
    }
    return rc;
}

int score_recompute(HyperMemory *hmem, CognitiveDomain domain, node_id_t subject_id) {
    if (!hmem || !hmem->txn) return -1;

    // 1. Собрать все Evaluation для данного subject_id
    NeuroAtom *atoms = NULL;
    size_t count = 0;
    // Ищем все атомы, где subject_id является участником (args[0])
    if (hyper_find_by_participant(hmem, subject_id, 0, &atoms, &count) != 0 || !atoms) {
        // нет наблюдений — удаляем Score, если он был? В RFC сказано: "если наблюдений нет — свёртка не создаётся/не трогается"
        // Пока просто ничего не делаем.
        free(atoms);
        return 0;
    }

    node_id_t obs_full = proc_make(djb2_hash("OBSERVED_OUTCOME"), PROC_KIND_EVENT);
    double sum_outcome = 0.0;
    int valid_count = 0;

    for (size_t i = 0; i < count; i++) {
        NeuroAtom *a = &atoms[i];
        if (a->process_id != obs_full) continue;
        if (HYPER_GET_TYPE(a->args[0].raw) != HYPER_TYPE_REF ||
            HYPER_GET_ID(a->args[0].raw) != subject_id) continue;
        if (HYPER_GET_TYPE(a->args[1].raw) != HYPER_TYPE_INT ||
            (CognitiveDomain)(uint64_t)HYPER_GET_ID(a->args[1].raw) != domain) continue;

        sum_outcome += a->truth_mean;
        valid_count++;
    }
    free(atoms);

    if (valid_count == 0) {
        // Наблюдений нет — удалять ли старый Score? По спецификации не трогаем.
        return 0;
    }

    // 2. Вычислить среднее (сегодня просто среднее; завтра можно любую формулу)
    float new_trust = (float)(sum_outcome / valid_count);
    float new_conf = 1.0f - expf(-0.1f * valid_count); // быстро растёт к 1.0

    // 3. Обновить или создать Score атом
    NeuroAtom *existing = find_score_atom(hmem, domain, subject_id);
    NeuroAtom score_atom;
    bool is_new = (existing == NULL);

    if (is_new) {
        memset(&score_atom, 0, sizeof(score_atom));
        score_atom.id            = hyper_memory_new_id(hmem);
        score_atom.process_id    = proc_make(djb2_hash(kHAS_SCORE), PROC_KIND_RELATION);
        score_atom.args[0].raw   = HYPER_MAKE_REF(subject_id);
        score_atom.args[1].raw   = (ko_id_t)(int64_t)domain | HYPER_TYPE_INT;
    } else {
        memcpy(&score_atom, existing, sizeof(NeuroAtom));
    }

    score_atom.truth_mean       = new_trust;
    score_atom.truth_confidence = new_conf;
    score_atom.sti              = 0.8f;
    score_atom.lti              = new_conf;
    score_atom.utility          = new_trust;
    score_atom.valence          = 0.0f;
    score_atom.context_or_time_link = 0;

    int rc = save_score(hmem, &score_atom);
    if (rc == 0) {
        LOG_PLANNER("[SCORE] Recomputed domain=%d subject=%lu trust=%.3f conf=%.3f (from %d obs)",
                    domain, (unsigned long)subject_id, new_trust, new_conf, valid_count);
    }
    return rc;
}

int score_propagate_credit(HyperMemory *hmem, CognitiveDomain domain,
                            node_id_t result_atom_id, float outcome,
                            uint32_t max_depth, float discount) {
    if (!hmem || !hmem->txn || !hmem->dbi_idx_causal) return -1;
    if (result_atom_id == 0) return -1;
    if (max_depth == 0)  max_depth = 8;
    if (max_depth > 64)  max_depth = 64; // защита от аномально длинных/циклических цепочек
    if (discount <= 0.0f || discount > 1.0f) discount = 0.7f;

    ko_id_t current_id = result_atom_id;
    uint32_t depth = 0;
    int propagated = 0;

    while (current_id != 0 && depth < max_depth) {
        MDB_val key = { sizeof(ko_id_t), &current_id };
        MDB_val val;

        if (mdb_get(hmem->txn, hmem->dbi_atoms, &key, &val) != MDB_SUCCESS ||
            val.mv_size != sizeof(NeuroAtom)) {
            break; // атом уже архивирован decay-циклом — обрываем трассировку, не ошибка
        }

        NeuroAtom atom;
        memcpy(&atom, val.mv_data, sizeof(NeuroAtom));

        float weight = powf(discount, (float)depth);

        for (int slot = 0; slot < HYPER_VAL_SLOTS; slot++) {
            if (HYPER_GET_TYPE(atom.args[slot].raw) != HYPER_TYPE_REF) continue;
            node_id_t subject = HYPER_GET_ID(atom.args[slot].raw);
            if (subject == 0) continue;

            if (score_update_weighted(hmem, domain, subject, outcome, weight,
                                       atom.id, atom.context_or_time_link) == 0)
                propagated++;
        }

        MDB_val cause_val;
        if (mdb_get(hmem->txn, hmem->dbi_idx_causal, &key, &cause_val) != MDB_SUCCESS ||
            cause_val.mv_size != sizeof(ko_id_t)) {
            break;
        }
        ko_id_t next_id;
        memcpy(&next_id, cause_val.mv_data, sizeof(ko_id_t));
        if (next_id == current_id) break; // защита от самопетли
        current_id = next_id;
        depth++;
    }

    return propagated;
}
