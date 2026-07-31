// reasoning/strategy_store.h
#pragma once

#include <lmdb.h>

#include "reasoning/strategy.h"

// Загружает текущие веса аналогии (persist через существующую db.graph.properties,
// без новых LMDB-таблиц). Если записи ещё нет — возвращает разумный дефолт.
void reasoning_weights_load(MDB_txn *txn, ReasoningWeights *out, uint32_t *out_step);

// SGD-шаг: x = {neighborhood, center, coverage, relation} признаки уже
// вычисленной аналогии, y = фактический исход (0/1 из score_propagate_credit).
void reasoning_weights_sgd_update(MDB_txn *txn, const float x[4], float y);