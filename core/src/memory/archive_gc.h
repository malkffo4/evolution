// memory/archive_gc.h
#pragma once

#include <lmdb.h>
#include <stdint.h>

#include "storage/hyper_atom/hyper_atom.h"

int archive_purge_cycle(MDB_txn *txn, HyperMemory *hmem, uint32_t max_age_sec, uint32_t *out_purged);
