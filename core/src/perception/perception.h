// perception/perception.h
#pragma once

#include <lmdb.h>

#include "memory/working.h"
#include "storage/hyper_atom/hyper_atom.h"

// Загрузка знаний из JSON напрямую в Рабочую Память (Working Memory)
int perceive_and_activate(const char *json_str, WorkingMemory *wm, MDB_txn *txn, HyperMemory *hmem);

int perceive_hyper_json(const char *json_str, MDB_txn *txn, HyperMemory *hmem);

int perceive_pipeline(const char *json_str, MDB_txn *txn);
