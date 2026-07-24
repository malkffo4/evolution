// storage/hyper_atom/hyper_atom.c
#include <stdbool.h>
#include <lmdb.h>
#include <stdlib.h>
#include <string.h>

#include "hyper_atom.h"

/* Инициализация — должна вызываться после открытия DBI в db.c */
HyperMemory *hyper_memory_new(MDB_txn *txn, MDB_dbi atoms, MDB_dbi idx_proc, MDB_dbi idx_args, MDB_dbi idx_ctx) {
    HyperMemory *mem = malloc(sizeof(HyperMemory));
    mem->txn = txn;
    mem->dbi_atoms = atoms;
    mem->dbi_idx_process = idx_proc;
    mem->dbi_idx_args = idx_args;
    mem->dbi_idx_context = idx_ctx;
    return mem;
}

void hyper_memory_free(HyperMemory *mem) {
    free(mem);
}

void hyper_memory_set_txn(HyperMemory *mem, MDB_txn *txn) {
    if (mem) mem->txn = txn;
}

// Проверка на существование атома с таким же process_id и аргументами
// (без учёта id, context_id и time_tick — только семантическая проверка)
static bool hyper_atom_exists(HyperMemory *mem, const HyperAtom *atom) {
    HyperAtom *existing = NULL;
    size_t count = 0;

    // Ищем по process_id
    if (hyper_find_by_process(mem, atom->process_id, 0, atom->context_id, &existing, &count) != 0)
        return false;

    for (size_t i = 0; i < count; i++) {
        // Сравниваем аргументы (все три)
        bool match = true;
        for (int a = 0; a < 3; a++) {
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
int hyper_assert_unique(HyperMemory *mem, const HyperAtom *atom) {
    if (!mem || !atom) return -1;

    // Проверяем, нет ли уже такого атома
    if (hyper_atom_exists(mem, atom)) {
        return 1;  // уже существует, не записываем
    }

    return hyper_assert(mem, atom);
}

int hyper_assert(HyperMemory *mem, const HyperAtom *atom) {
    if (!mem || !atom)
        return -1;

    MDB_val key_id = {sizeof(ko_id_t), (void *)&atom->id};
    MDB_val val_atom = {sizeof(HyperAtom), (void *)atom};

    int rc = mdb_put(mem->txn, mem->dbi_atoms, &key_id, &val_atom, 0);
    if (rc != MDB_SUCCESS)
        return rc;

    /* Индекс process_id → id */
    MDB_val key_proc = {sizeof(ko_id_t), (void *)&atom->process_id};
    mdb_put(mem->txn, mem->dbi_idx_process, &key_proc, &key_id, 0); // не APPENDDUP

    // БЕЗОПАСНАЯ ИНДЕКСАЦИЯ АРГУМЕНТОВ
    // Кладем в индекс ТОЛЬКО если это ссылка, предварительно очистив маску типа.
    for (int i = 0; i < 3; i++) {
        if (HYPER_GET_TYPE(atom->args[i].raw) == HYPER_TYPE_REF && atom->args[i].raw != 0) {
            ko_id_t clean_ref = HYPER_GET_ID(atom->args[i].raw);
            MDB_val key_arg = {sizeof(ko_id_t), (void *)&clean_ref};
            mdb_put(mem->txn, mem->dbi_idx_args, &key_arg, &key_id, MDB_APPENDDUP);
        }
    }

    /* Индекс контекст → id */
    MDB_val key_ctx = {sizeof(ko_id_t), (void *)&atom->context_id};
    mdb_put(mem->txn, mem->dbi_idx_context, &key_ctx, &key_id, MDB_APPENDDUP);

    return 0;
}

int hyper_find_by_process(HyperMemory *mem, ko_id_t process_id, ko_id_t participant_id, ko_id_t context_id, HyperAtom **results, size_t *count) {
    MDB_cursor *cursor;
    MDB_val key = {sizeof(ko_id_t), &process_id};
    MDB_val val_id;
    if (mdb_cursor_open(mem->txn, mem->dbi_idx_process, &cursor) != MDB_SUCCESS) return -1;

    *count = 0;
    size_t capacity = 16;
    *results = malloc(sizeof(HyperAtom) * capacity);

    int rc = mdb_cursor_get(cursor, &key, &val_id, MDB_SET);
    while (rc == MDB_SUCCESS) {
        MDB_val val_atom;
        if (mdb_get(mem->txn, mem->dbi_atoms, &val_id, &val_atom) == MDB_SUCCESS) {
            HyperAtom *atom = (HyperAtom *)val_atom.mv_data;
            // Фильтр по контексту
            if (context_id != 0 && atom->context_id != context_id) {
                rc = mdb_cursor_get(cursor, &key, &val_id, MDB_NEXT_DUP);
                continue;
            }
            // Если задан participant_id, проверяем любой из трёх аргументов
            if (participant_id != 0) {
                bool found = false;
                for (int i = 0; i < 3; i++) {
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
                *results = realloc(*results, sizeof(HyperAtom) * capacity);
            }
            memcpy(&(*results)[*count], atom, sizeof(HyperAtom));
            (*count)++;
        }
        rc = mdb_cursor_get(cursor, &key, &val_id, MDB_NEXT_DUP);
    }
    mdb_cursor_close(cursor);
    return 0;
}

int hyper_find_by_participant(HyperMemory *mem, ko_id_t participant_id, ko_id_t context_id, HyperAtom **results, size_t *count) {
    MDB_cursor *cursor;
    MDB_val key = { sizeof(ko_id_t), &participant_id };
    MDB_val val_id;

    if (mdb_cursor_open(mem->txn, mem->dbi_idx_args, &cursor) != MDB_SUCCESS) return -1;

    *count = 0;
    size_t capacity = 16;
    *results = malloc(sizeof(HyperAtom) * capacity);

    int rc = mdb_cursor_get(cursor, &key, &val_id, MDB_SET);
    while (rc == MDB_SUCCESS) {
        MDB_val val_atom;
        if (mdb_get(mem->txn, mem->dbi_atoms, &val_id, &val_atom) == MDB_SUCCESS) {
            HyperAtom *atom = (HyperAtom*)val_atom.mv_data;
            if (context_id == 0 || atom->context_id == context_id) {
                if (*count >= capacity) {
                    capacity *= 2;
                    *results = realloc(*results, sizeof(HyperAtom) * capacity);
                }
                memcpy(&(*results)[*count], atom, sizeof(HyperAtom));
                (*count)++;
            }
        }
        rc = mdb_cursor_get(cursor, &key, &val_id, MDB_NEXT_DUP);
    }
    mdb_cursor_close(cursor);
    return 0;
}

// Трассировка причинности остается почти без изменений
int hyper_trace_cause(HyperMemory *mem, ko_id_t start_id, HyperAtom **chain, size_t max_depth, size_t *count) {
    if (!mem || !chain || !count)
        return -1;

    *chain = malloc(sizeof(HyperAtom) * max_depth);
    *count = 0;
    ko_id_t current_id = start_id;

    while (*count < max_depth && current_id != 0) {
        MDB_val key = {sizeof(ko_id_t), &current_id};
        MDB_val val;

        if (mdb_get(mem->txn, mem->dbi_atoms, &key, &val) != MDB_SUCCESS)
            break;

        HyperAtom *atom = (HyperAtom *)val.mv_data;
        memcpy(&(*chain)[*count], atom, sizeof(HyperAtom));
        (*count)++;

        current_id = atom->cause_id;
    }
    return 0;
}
