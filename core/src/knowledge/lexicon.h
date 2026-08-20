// knowledge/lexicon.h
#pragma once

#include <lmdb.h>

#include "types/id.h"
#include "storage/hyper_atom/hyper_atom.h"

int lexicon_is_grounded(MDB_txn *txn, HyperMemory *hmem, node_id_t word_id);
// проверяет наличие COMPOSED_OF(word_id, *) — уже объяснено через примитивы

void lexicon_mark_ungrounded(MDB_txn *txn, HyperMemory *hmem, node_id_t word_id);
// event_queue_push в "genesis::ungrounded", если ещё не заземлено
