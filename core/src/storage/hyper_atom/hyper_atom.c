// storage/hyper_atom/hyper_atom.c
#include <stdbool.h>
#include <lmdb.h>
#include <stdlib.h>
#include <string.h>

#include <stdatomic.h>

#include "math/vector_math.h"
#include "storage/vector_store/vector_store.h"
#include "storage/db/db.h"
#include "hyper_atom.h"

ko_id_t hyper_memory_new_id(HyperMemory *mem) {
    if (!mem || !mem->idgen) return 0;
    uint64_t seq = atomic_fetch_add(&mem->idgen->counter,1);

    return ((uint64_t)mem->idgen->node_id << 48) | ((uint64_t)mem->idgen->session_id << 32) | seq;
}
// TODO. Для системы, которая должна жить месяцами, учиться, исполнять алгоритмы и сохранять историю, я бы сделал:
// node_id | boot/session_id | monotonic sequence
// причём node_id должен быть постоянным для конкретного экземпляра Core, а session — новым при каждом запуске.
// Иначе persistence начинает зависеть от того, какой HyperMemory первым получил какой ID.
// В vm_pool это частично исправлено установкой session_id:
// worker_hmem->idgen->session_id = (uint16_t)((job->goal_id ^ job->algo_id) & 0xFFFF);
// Но это всё ещё не полноценная глобальная идентичность.
HyperMemory *hyper_memory_new(MDB_dbi atoms, MDB_dbi idx_proc, MDB_dbi idx_args, MDB_dbi idx_ctx) {
    HyperMemory *mem = calloc(1, sizeof(HyperMemory));
    if (!mem) return NULL;
    mem->idgen = calloc(1, sizeof(HyperIdGenerator));
    if (!mem->idgen) {
        free(mem);
        return NULL;
    }
    mem->idgen->node_id = 0;
    mem->idgen->session_id = 0;
    atomic_store(&mem->idgen->counter, 1);

    mem->dbi_atoms = atoms;
    mem->dbi_idx_process = idx_proc;
    mem->dbi_idx_args = idx_args;
    mem->dbi_idx_context = idx_ctx;
    return mem;
}

void hyper_memory_free(HyperMemory *mem) {
    if (mem) {
        free(mem->idgen);
        free(mem);
    }
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

int hyper_find_by_process(MDB_txn *txn, HyperMemory *mem,
                          ko_id_t process_id, ko_id_t participant_id,
                          ko_id_t context_id, NeuroAtom **results, size_t *count) {
    if (!mem || !txn) return -1;
    // Делегируем вызов функции с STI-фильтром, установив порог в 0.0f (искать всё)
    return hyper_find_by_process_sti(txn, mem, process_id, participant_id, context_id, 0.0f, results, count);
}

// Проверка на существование атома с таким же process_id и аргументами
// (без учёта id, context_id и time_tick — только семантическая проверка)
static bool hyper_atom_exists(MDB_txn *txn, HyperMemory *mem, const NeuroAtom *atom) {
    if (!mem || !txn) return false;
    NeuroAtom *existing = NULL;
    size_t count = 0;
    if (hyper_find_by_process(txn, mem, atom->process_id, 0, atom->context_or_time_link, &existing, &count) != 0)
        return false;

    for (size_t i = 0; i < count; i++) {
        bool match = true;
        for (int a = 0; a < HYPER_VAL_SLOTS; a++) {
            if (existing[i].args[a].raw != atom->args[a].raw) {
                match = false;
                break;
            }
        }
        if (match) {
            // ФИКС: Обновляем ID, чтобы вызывающая сторона (OP_ASSERT/DERIVE)
            // использовала реальный узел, а не "повисший" фантомный ID.
            ((NeuroAtom *)atom)->id = existing[i].id;
            free(existing);
            return true;
        }
    }
    free(existing);
    return false;
}

int hyper_assert_unique(MDB_txn *txn, HyperMemory *mem, const NeuroAtom *atom) {
    if (!txn || !mem || !atom) return -1;
    if (hyper_atom_exists(txn, mem, atom)) return 1;
    return hyper_assert(txn, mem, atom);
}

int hyper_assert(MDB_txn *txn, HyperMemory *mem, const NeuroAtom *atom) {
    if (!txn || !mem || !atom) return -1;
    MDB_val key_id = {sizeof(ko_id_t), (void *)&atom->id};
    MDB_val val_atom = {sizeof(NeuroAtom), (void *)atom};

    int rc = mdb_put(txn, mem->dbi_atoms, &key_id, &val_atom, 0);
    if (rc != MDB_SUCCESS) return rc;

    MDB_val key_proc = {sizeof(ko_id_t), (void *)&atom->process_id};
    rc = mdb_put(txn, mem->dbi_idx_process, &key_proc, &key_id, 0);
    if (rc != MDB_SUCCESS) return rc;

    for (int i = 0; i < HYPER_VAL_SLOTS; i++) {
        if (HYPER_GET_ID(atom->args[i].raw) == 0)
            continue;

        ko_id_t clean_ref = HYPER_GET_ID(atom->args[i].raw);

        MDB_val key_arg = {
            sizeof(ko_id_t),
            (void *)&clean_ref
        };

        rc = mdb_put(txn, mem->dbi_idx_args, &key_arg, &key_id, 0);
        if (rc != MDB_SUCCESS)
            return rc;
    }

    MDB_val key_ctx = {
        sizeof(ko_id_t),
        (void *)&atom->context_or_time_link
    };

    rc = mdb_put(txn, mem->dbi_idx_context, &key_ctx, &key_id, 0);
    if (rc != MDB_SUCCESS)
        return rc;

    return MDB_SUCCESS;
}

int hyper_assert_with_cause(MDB_txn *txn, HyperMemory *mem, const NeuroAtom *atom, ko_id_t cause_id) {
    if (!txn || !mem || !atom) return -1;
    int rc = hyper_assert_unique(txn, mem, atom);
    if (rc != 0 && rc != 1) return rc;

    if (cause_id != 0) {
        if (mem->dbi_idx_causal) {
            MDB_val k_child = { sizeof(ko_id_t), (void *)&atom->id };
            MDB_val v_cause = { sizeof(ko_id_t), (void *)&cause_id };
            mdb_put(txn, mem->dbi_idx_causal, &k_child, &v_cause, 0);
        }

        // Записываем связь в обратный индекс, чтобы eval_graph
        // мог переходить к следующей инструкции сгенерированного графа!
        MDB_val k_rev = { sizeof(ko_id_t), (void *)&cause_id };
        MDB_val v_rev = { sizeof(ko_id_t), (void *)&atom->id };
        mdb_put(txn, db.graph.hyper.idx_causal_rev, &k_rev, &v_rev, 0);
    }
    return rc;
}

int hyper_find_by_process_sti(MDB_txn *txn, HyperMemory *mem,
                        ko_id_t process_id, ko_id_t participant_id,
                        ko_id_t context_id, float sti_threshold,
                        NeuroAtom **results, size_t *count) {
    if (!mem || !txn) return -1;
    MDB_cursor *cursor;
    MDB_val key = {sizeof(ko_id_t), &process_id};
    MDB_val val_id;
    if (mdb_cursor_open(txn, mem->dbi_idx_process, &cursor) != MDB_SUCCESS) return -1;

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
        if (mdb_get(txn, mem->dbi_atoms, &val_id, &val_atom) == MDB_SUCCESS) {
            NeuroAtom *atom = (NeuroAtom *)val_atom.mv_data;
            // Фильтр по контексту
            if (context_id != 0 && atom->context_or_time_link != context_id) {
                rc = mdb_cursor_get(cursor, &key, &val_id, MDB_NEXT_DUP);
                continue;
            }

            // Фильтр по порогу STI (кратковременная память)
            if (atom->sti < sti_threshold) {
                rc = mdb_cursor_get(cursor, &key, &val_id, MDB_NEXT_DUP);
                continue;
            }

            // Если задан participant_id, проверяем любой из трёх аргументов
            if (participant_id != 0) {
                bool found = false;
                for (int i = 0; i < HYPER_VAL_SLOTS; i++) {
                    if (HYPER_GET_ID(atom->args[i].raw) == participant_id) {
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

int hyper_find_by_participant(MDB_txn *txn, HyperMemory *mem,
                        ko_id_t participant_id, ko_id_t context_id,
                        NeuroAtom **results, size_t *count) {
    if (!mem || !txn) return -1;
    MDB_cursor *cursor;
    MDB_val key = { sizeof(ko_id_t), &participant_id };
    MDB_val val_id;

    if (mdb_cursor_open(txn, mem->dbi_idx_args, &cursor) != MDB_SUCCESS) return -1;

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
        if (mdb_get(txn, mem->dbi_atoms, &val_id, &val_atom) == MDB_SUCCESS) {
            NeuroAtom *atom = (NeuroAtom*)val_atom.mv_data;
            if (context_id == 0 || atom->context_or_time_link == context_id) {
                if (*count >= capacity) {
                    capacity *= 2;
                    NeuroAtom *grown = realloc(*results, sizeof(NeuroAtom) * capacity);
                    if (!grown) { mdb_cursor_close(cursor); return -1; }
                    *results = grown;
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

int hyper_find_by_context(MDB_txn *txn, HyperMemory *mem, ko_id_t context_id,
                           NeuroAtom **results, size_t *count) {
    if (!mem || !txn) return -1;
    MDB_cursor *cursor;
    MDB_val key = { sizeof(ko_id_t), &context_id };
    MDB_val val_id;

    if (mdb_cursor_open(txn, mem->dbi_idx_context, &cursor) != MDB_SUCCESS) return -1;

    *count = 0;
    size_t capacity = 16;
    *results = malloc(sizeof(NeuroAtom) * capacity);
    if (!*results) { mdb_cursor_close(cursor); return -1; }

    int rc = mdb_cursor_get(cursor, &key, &val_id, MDB_SET);
    while (rc == MDB_SUCCESS) {
        MDB_val val_atom;
        if (mdb_get(txn, mem->dbi_atoms, &val_id, &val_atom) == MDB_SUCCESS) {
            if (*count >= capacity) {
                capacity *= 2;
                NeuroAtom *grown = realloc(*results, sizeof(NeuroAtom) * capacity);
                if (!grown) { mdb_cursor_close(cursor); return -1; }
                *results = grown;
            }
            memcpy(&(*results)[*count], val_atom.mv_data, sizeof(NeuroAtom));
            (*count)++;
        }
        rc = mdb_cursor_get(cursor, &key, &val_id, MDB_NEXT_DUP);
    }
    mdb_cursor_close(cursor);
    return 0;
}

// Трассировка причинности
int hyper_trace_cause(MDB_txn *txn, HyperMemory *mem, ko_id_t start_id, NeuroAtom **chain, size_t max_depth, size_t *count) {
    if (!mem || !txn || !chain || !count) return -1;

    // Если запрошен дефолт (0), ставим разумный лимит глубины
    if (max_depth == 0) max_depth = 64;

    *chain = malloc(sizeof(NeuroAtom) * max_depth);
    if (!*chain) return -1;
    *count = 0;
    ko_id_t current_id = start_id;

    while (*count < max_depth && current_id != 0) {
        // Загружаем текущий атом
        MDB_val key = {sizeof(ko_id_t), &current_id};
        MDB_val val;
        if (mdb_get(txn, mem->dbi_atoms, &key, &val) != MDB_SUCCESS) {
            // ФОЛБЭК: ищем в архиве, так как инструкции и старые факты могут быть там
            if (!mem->dbi_archive || mdb_get(txn, mem->dbi_archive, &key, &val) != MDB_SUCCESS) {
                break;
            }
        }

        NeuroAtom *atom = (NeuroAtom *)val.mv_data;
        memcpy(&(*chain)[*count], atom, sizeof(NeuroAtom));
        (*count)++;

        // Переходим к причине (cause_id) текущего атома через индекс
        MDB_val cause_val;
        if (mdb_get(txn, mem->dbi_idx_causal, &key, &cause_val) != MDB_SUCCESS)
            break;   // нет причины, завершаем

        current_id = *(ko_id_t *)cause_val.mv_data;  // следующий атом в цепочке
    }

    return 0;
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
