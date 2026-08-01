// storage/vector_store/vector_store.h
#pragma once

#include <stdint.h>
#include <lmdb.h>
#include "storage/hyper_atom/hyper_atom.h" // Подключаем для VECTOR_DIM и ko_id_t

#define HASH_BITS           256
#define HAMMING_THRESHOLD   30
#define MAX_CANDIDATES      1000

// Инициализация проекционной матрицы
int init_simhash(MDB_txn *txn);

// Вычисление 256-битного SimHash из вектора
void compute_simhash256(const float *embedding, uint64_t hash[4]);

// Сохраняет вектор в единую таблицу idx_vectors и обновляет simhash_index
int save_embedding(MDB_txn *txn, ko_id_t node_id, const float *emb);

// Читает вектор из единой таблицы idx_vectors
int load_embedding(MDB_txn *txn, ko_id_t node_id, float *emb_out);

// Быстрый LSH-поиск (возвращает отсортированный массив ID)
int find_similar_nodes(MDB_txn *txn, const float *query_emb, int topK, uint64_t *results);
