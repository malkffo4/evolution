// storage/hyper_atom/hyper_atom.c
#include <stdbool.h>
#include <lmdb.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>

#include "storage/vector_store/vector_store.h"
#include "storage/db/db.h"
#include "hyper_atom.h"

ko_id_t hyper_memory_new_id(HyperMemory *mem) {
    uint64_t seq = atomic_fetch_add(&mem->idgen->counter,1);

    return ((uint64_t)mem->idgen->node_id << 48) | ((uint64_t)mem->idgen->session_id << 32) | seq;
}

/* Инициализация — должна вызываться после открытия DBI в db.c */
HyperMemory *hyper_memory_new(MDB_txn *txn, MDB_dbi atoms, MDB_dbi idx_proc, MDB_dbi idx_args, MDB_dbi idx_ctx) {
    HyperMemory *mem = calloc(1, sizeof(HyperMemory));
    if (!mem) return NULL;

    mem->idgen = calloc(1, sizeof(HyperIdGenerator));
    if (!mem->idgen) {
        free(mem);
        return NULL;
    }
    // Настройки по умолчанию: можно позже передавать через аргументы
    mem->idgen->node_id = 0;
    mem->idgen->session_id = 0;
    atomic_store(&mem->idgen->counter, 1);

    mem->txn = txn;
    mem->dbi_atoms = atoms;
    mem->dbi_idx_process = idx_proc;
    mem->dbi_idx_args = idx_args;
    mem->dbi_idx_context = idx_ctx;
    // dbi_idx_causal, dbi_archive, dbi_idx_vectors останутся 0
    return mem;
}

void hyper_memory_free(HyperMemory *mem) {
    if (mem) {
        free(mem->idgen);
        free(mem);
    }
}

void hyper_memory_set_txn(HyperMemory *mem, MDB_txn *txn) {
    if (mem) mem->txn = txn;
}
void hyper_memory_set_db_archive(HyperMemory *mem, MDB_dbi archive) {
    if (mem) mem->dbi_archive = archive;
}
void hyper_memory_set_db_causal(HyperMemory *mem, MDB_dbi causal) {
    if (mem) mem->dbi_idx_causal = causal;
}
void hyper_memory_set_db_vectors(HyperMemory *mem, MDB_dbi vectors) {
    if (mem) mem->dbi_idx_vectors = vectors;
}

// Проверка на существование атома с таким же process_id и аргументами
// (без учёта id, context_id и time_tick — только семантическая проверка)
static bool hyper_atom_exists(HyperMemory *mem, const NeuroAtom *atom) {
    NeuroAtom *existing = NULL;
    size_t count = 0;

    // Ищем по process_id
    if (hyper_find_by_process(mem, atom->process_id, 0, atom->context_or_time_link, &existing, &count) != 0)
        return false;

    for (size_t i = 0; i < count; i++) {
        // Сравниваем аргументы
        bool match = true;
        for (int a = 0; a < HYPER_VAL_SLOTS; a++) {
            if (existing[i].args[a].raw != atom->args[a].raw) {
                match = false;
                break;
            }
        }
        if (match) {
            free(existing);
            return true;  // найден дубликат
        }
    }

    free(existing);
    return false;
}

// Модифицируем hyper_assert с флагом проверки дубликатов
int hyper_assert_unique(HyperMemory *mem, const NeuroAtom *atom) {
    if (!mem || !atom) return -1;

    // Проверяем, нет ли уже такого атома
    if (hyper_atom_exists(mem, atom)) {
        return 1;  // уже существует, не записываем
    }

    return hyper_assert(mem, atom);
}

int hyper_assert(HyperMemory *mem, const NeuroAtom *atom) {
    if (!mem || !atom) return -1;

    MDB_val key_id = {sizeof(ko_id_t), (void *)&atom->id};
    MDB_val val_atom = {sizeof(NeuroAtom), (void *)atom};

    int rc = mdb_put(mem->txn, mem->dbi_atoms, &key_id, &val_atom, 0);
    if (rc != MDB_SUCCESS) return rc;

    MDB_val key_proc = {sizeof(ko_id_t), (void *)&atom->process_id};
    mdb_put(mem->txn, mem->dbi_idx_process, &key_proc, &key_id, 0);

    for (int i = 0; i < HYPER_VAL_SLOTS; i++) {  // теперь только 2 слота args
        if (HYPER_GET_TYPE(atom->args[i].raw) == HYPER_TYPE_REF && atom->args[i].raw != 0) {
            ko_id_t clean_ref = HYPER_GET_ID(atom->args[i].raw);
            MDB_val key_arg = {sizeof(ko_id_t), (void *)&clean_ref};
            mdb_put(mem->txn, mem->dbi_idx_args, &key_arg, &key_id, 0);
        }
    }

    MDB_val key_ctx = {sizeof(ko_id_t), (void *)&atom->context_or_time_link};
    mdb_put(mem->txn, mem->dbi_idx_context, &key_ctx, &key_id, 0);

    return 0;
}

int hyper_find_by_process(HyperMemory *mem, ko_id_t process_id, ko_id_t participant_id, ko_id_t context_id, NeuroAtom **results, size_t *count) {
    MDB_cursor *cursor;
    MDB_val key = {sizeof(ko_id_t), &process_id};
    MDB_val val_id;
    if (mdb_cursor_open(mem->txn, mem->dbi_idx_process, &cursor) != MDB_SUCCESS) return -1;

    *count = 0;
    size_t capacity = 16;
    *results = malloc(sizeof(NeuroAtom) * capacity);
    if (!*results) {
        mdb_cursor_close(cursor);
        return -1;
    }
    int rc = mdb_cursor_get(cursor, &key, &val_id, MDB_SET);
    while (rc == MDB_SUCCESS) {
        MDB_val val_atom;
        if (mdb_get(mem->txn, mem->dbi_atoms, &val_id, &val_atom) == MDB_SUCCESS) {
            NeuroAtom *atom = (NeuroAtom *)val_atom.mv_data;
            // Фильтр по контексту
            if (context_id != 0 && atom->context_or_time_link != context_id) {
                rc = mdb_cursor_get(cursor, &key, &val_id, MDB_NEXT_DUP);
                continue;
            }
            // Если задан participant_id, проверяем любой из трёх аргументов
            if (participant_id != 0) {
                bool found = false;
                for (int i = 0; i < HYPER_VAL_SLOTS; i++) {
                    if (HYPER_GET_TYPE(atom->args[i].raw) == HYPER_TYPE_REF &&
                        HYPER_GET_ID(atom->args[i].raw) == participant_id) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    rc = mdb_cursor_get(cursor, &key, &val_id, MDB_NEXT_DUP);
                    continue;
                }
            }
            // Добавляем в результат
            if (*count >= capacity) {
                capacity *= 2;
                *results = realloc(*results, sizeof(NeuroAtom) * capacity);
                if (!*results) {
                    mdb_cursor_close(cursor);
                    return -1;
                }
            }
            memcpy(&(*results)[*count], atom, sizeof(NeuroAtom));
            (*count)++;
        }
        rc = mdb_cursor_get(cursor, &key, &val_id, MDB_NEXT_DUP);
    }
    mdb_cursor_close(cursor);
    return 0;
}

int hyper_find_by_participant(HyperMemory *mem, ko_id_t participant_id, ko_id_t context_id, NeuroAtom **results, size_t *count) {
    MDB_cursor *cursor;
    MDB_val key = { sizeof(ko_id_t), &participant_id };
    MDB_val val_id;

    if (mdb_cursor_open(mem->txn, mem->dbi_idx_args, &cursor) != MDB_SUCCESS) return -1;

    *count = 0;
    size_t capacity = 16;
    *results = malloc(sizeof(NeuroAtom) * capacity);
    if (!*results) {
        mdb_cursor_close(cursor);
        return -1;
    }
    int rc = mdb_cursor_get(cursor, &key, &val_id, MDB_SET);
    while (rc == MDB_SUCCESS) {
        MDB_val val_atom;
        if (mdb_get(mem->txn, mem->dbi_atoms, &val_id, &val_atom) == MDB_SUCCESS) {
            NeuroAtom *atom = (NeuroAtom*)val_atom.mv_data;
            if (context_id == 0 || atom->context_or_time_link == context_id) {
                if (*count >= capacity) {
                    capacity *= 2;
                    *results = realloc(*results, sizeof(NeuroAtom) * capacity);
                    if (!*results) {
                        mdb_cursor_close(cursor);
                        return -1;
                    }
                }
                memcpy(&(*results)[*count], atom, sizeof(NeuroAtom));
                (*count)++;
            }
        }
        rc = mdb_cursor_get(cursor, &key, &val_id, MDB_NEXT_DUP);
    }
    mdb_cursor_close(cursor);
    return 0;
}

// Трассировка причинности
int hyper_trace_cause(HyperMemory *mem, ko_id_t start_id, NeuroAtom **chain, size_t max_depth, size_t *count) {
    if (!mem || !chain || !count) return -1;

    *chain = malloc(sizeof(NeuroAtom) * max_depth);
    if (!*chain) return -1;
    *count = 0;
    ko_id_t current_id = start_id;

    while (*count < max_depth && current_id != 0) {
        // Загружаем текущий атом
        MDB_val key = {sizeof(ko_id_t), &current_id};
        MDB_val val;
        if (mdb_get(mem->txn, mem->dbi_atoms, &key, &val) != MDB_SUCCESS)
            break;

        NeuroAtom *atom = (NeuroAtom *)val.mv_data;
        memcpy(&(*chain)[*count], atom, sizeof(NeuroAtom));
        (*count)++;

        // Переходим к причине (cause_id) текущего атома через индекс
        MDB_val cause_val;
        if (mdb_get(mem->txn, mem->dbi_idx_causal, &key, &cause_val) != MDB_SUCCESS)
            break;   // нет причины, завершаем

        current_id = *(ko_id_t *)cause_val.mv_data;  // следующий атом в цепочке
    }

    return 0;
}

int hyper_assert_with_cause(HyperMemory *mem, const NeuroAtom *atom, ko_id_t cause_id) {
    int rc = hyper_assert_unique(mem, atom);
    if (rc != 0 && rc != 1) return rc;   // ошибка (не считаем "уже существует" ошибкой)

    if (cause_id != 0 && mem->dbi_idx_causal) {
        MDB_val k_child = { sizeof(ko_id_t), (void *)&atom->id };
        MDB_val v_cause = { sizeof(ko_id_t), (void *)&cause_id };

        // Прямая связь (child -> cause)
        mdb_put(mem->txn, mem->dbi_idx_causal, &k_child, &v_cause, MDB_APPENDDUP);

        // Обратная связь (cause -> child) для сверхбыстрого ремаппинга в OP_MERGE_CTX
        if (db.graph.hyper.idx_causal_rev) {
            mdb_put(mem->txn, db.graph.hyper.idx_causal_rev, &v_cause, &k_child, MDB_APPENDDUP);
        }
    }
    return rc;
}

int hyper_vector_save(MDB_txn *txn, MDB_dbi dbi, ko_id_t atom_id, const Vector128 *vec) {
    (void)dbi; // Игнорируем dbi, save_embedding сам пишет в idx_vectors и simhash_index
    if (!txn || !vec) return -1;
    return save_embedding(txn, atom_id, vec->data);
}

int hyper_vector_load(MDB_txn *txn, MDB_dbi dbi, ko_id_t atom_id, Vector128 *out) {
    if (!txn || !out) return -1;
    MDB_val key = { sizeof(ko_id_t), (void*)&atom_id };
    MDB_val data;
    int rc = mdb_get(txn, dbi, &key, &data);
    if (rc != MDB_SUCCESS) return rc;
    if (data.mv_size != sizeof(Vector128)) return -1;
    memcpy(out, data.mv_data, sizeof(Vector128));
    return 0;
}

float vector_cosine_similarity(const Vector128 *a, const Vector128 *b) {
    float dot = 0.0f, norm_a = 0.0f, norm_b = 0.0f;
    for (int i = 0; i < VECTOR_DIM; i++) {
        dot   += a->data[i] * b->data[i];
        norm_a += a->data[i] * a->data[i];
        norm_b += b->data[i] * b->data[i];
    }
    if (norm_a < 1e-8f || norm_b < 1e-8f) return 0.0f;
    return dot / (sqrtf(norm_a) * sqrtf(norm_b));
}

int hyper_find_by_process_sti(HyperMemory *mem, ko_id_t process_id,
                               ko_id_t participant_id, ko_id_t context_id,
                               float sti_threshold,
                               NeuroAtom **results, size_t *count) {
    MDB_cursor *cursor;
    MDB_val key = { sizeof(ko_id_t), &process_id };
    MDB_val val_id;
    if (mdb_cursor_open(mem->txn, mem->dbi_idx_process, &cursor) != MDB_SUCCESS)
        return -1;

    *count = 0;
    size_t capacity = 16;
    *results = malloc(sizeof(NeuroAtom) * capacity);
    if (!*results) {
        mdb_cursor_close(cursor);
        return -1;
    }

    int rc = mdb_cursor_get(cursor, &key, &val_id, MDB_SET);
    while (rc == MDB_SUCCESS) {
        MDB_val val_atom;
        if (mdb_get(mem->txn, mem->dbi_atoms, &val_id, &val_atom) == MDB_SUCCESS) {
            NeuroAtom *atom = (NeuroAtom *)val_atom.mv_data;

            // ── STI‑фильтр (самый дешёвый – отсекаем холодные атомы сразу) ──
            if (sti_threshold > 0.0f && atom->sti < sti_threshold) {
                rc = mdb_cursor_get(cursor, &key, &val_id, MDB_NEXT_DUP);
                continue;
            }

            // ── Фильтр по контексту ──
            if (context_id != 0 && atom->context_or_time_link != context_id) {
                rc = mdb_cursor_get(cursor, &key, &val_id, MDB_NEXT_DUP);
                continue;
            }

            // ── Фильтр по участнику (проверяем оба слота args) ──
            if (participant_id != 0) {
                bool found = false;
                for (int i = 0; i < HYPER_VAL_SLOTS; i++) {
                    if (HYPER_GET_TYPE(atom->args[i].raw) == HYPER_TYPE_REF &&
                        HYPER_GET_ID(atom->args[i].raw) == participant_id) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    rc = mdb_cursor_get(cursor, &key, &val_id, MDB_NEXT_DUP);
                    continue;
                }
            }

            // ── Добавляем в результат ──
            if (*count >= capacity) {
                capacity *= 2;
                NeuroAtom *tmp = realloc(*results, sizeof(NeuroAtom) * capacity);
                if (!tmp) {
                    free(*results);
                    *results = NULL;
                    mdb_cursor_close(cursor);
                    return -1;
                }
                *results = tmp;
            }
            memcpy(&(*results)[*count], atom, sizeof(NeuroAtom));
            (*count)++;
        }
        rc = mdb_cursor_get(cursor, &key, &val_id, MDB_NEXT_DUP);
    }

    mdb_cursor_close(cursor);
    return 0;
}
