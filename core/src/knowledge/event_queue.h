// knowledge/event_queue.h
#pragma once

#include <lmdb.h>

#include "types/id.h"
#include "storage/hyper_atom/hyper_atom.h"

#define EVENT_QUEUE_MAX_POP 256

// Кладёт atom_id в очередь queue_id. НЕ идемпотентно относительно дублей —
// один и тот же атом может законно попасть в две независимые очереди.
int event_queue_push(MDB_txn *txn, HyperMemory *hmem, ko_id_t queue_id, ko_id_t atom_id);

// Извлекает до max_count элементов (at-most-once: физически удаляет их
// из очереди этим же вызовом). Порядок практически хронологический,
// потому что hyper_memory_new_id() монотонно растёт по (timestamp<<32|seq).
// Возвращает количество извлечённых (0..max_count) или -1 при ошибке.
int event_queue_pop_batch(MDB_txn *txn, HyperMemory *hmem, ko_id_t queue_id,
                           ko_id_t *out_atom_ids, uint32_t max_count);

uint32_t event_queue_len(MDB_txn *txn, HyperMemory *hmem, ko_id_t queue_id);
