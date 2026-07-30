// knowledge/episode.h
#ifndef KNOWLEDGE_EPISODE_H
#define KNOWLEDGE_EPISODE_H

#include <stdint.h>
#include <lmdb.h>

#include "types/id.h"
#include "storage/hyper_atom/hyper_atom.h"
#include "runtime/vm/vm_status.h"

/*
 * Episode — неизменяемая запись одного цикла Goal -> Algorithm -> Execution -> Result.
 *
 * Использует уже объявленную, но ранее не задействованную таблицу
 * db.memory.episodes (storage/db/db.h/db.c). Новых LMDB-таблиц не создаётся.
 *
 * Хранится как raw struct той же схемой, что Pipeline в
 * knowledge/algorithm_saver.c: полный блоб в db.memory.episodes,
 * ключ = Episode.id.
 *
 * Дополнительно публикуется один NeuroAtom-указатель
 * (process = EPISODE_RECORDED, PROC_KIND_EVENT):
 *   id      = episode_id  (== ключ в db.memory.episodes)
 *   args[0] = REF(goal_id)
 *   args[1] = REF(algorithm_id)
 *   truth_mean = outcome
 * Благодаря этому "все эпизоды по цели" и "все эпизоды по алгоритму"
 * находятся штатным hyper_find_by_participant() без новых индексов.
 *
 * Потокобезопасность: episode_record() пишет в hmem->txn — обязана
 * вызываться только внутри write-транзакции db_writer потока.
 *
 * Replay: Episode неизменяем после записи (см. docs/EVENT_MODEL.md:
 * "Events are append-only. They are never modified after publication").
 *
 * Фундамент для Critic/Self-Correction (следующий этап): Critic читает
 * episode_load() по id, найденным через hyper_find_by_participant(goal_id
 * / algorithm_id), и группирует их для Pattern Discovery (docs/09_Learning.md).
 */
typedef struct {
    ko_id_t   id;                 // == ключ в db.memory.episodes == id указателя-атома
    ko_id_t   goal_id;
    ko_id_t   algorithm_id;
    ko_id_t   result_atom_id;     // атом, выведенный OP_ASSERT/OP_DERIVE во время
                                   // исполнения (0, если алгоритм ничего не вывел)
    ko_id_t   context_id;         // ctx->current_context на момент исполнения
    int32_t   vm_status;          // VMStatus, как int32 для стабильного размера
    float     outcome;            // 1.0 успех / 0.0 неудача — то же, что в score_update()
    uint64_t  start_cycles;       // vm_rdtsc() до vm_execute()
    uint64_t  duration_cycles;    // дельта vm_rdtsc() (0 на платформах без TSC — см. time.c)
    uint64_t  wall_time;          // time(NULL) — для человекочитаемых логов
} Episode;

/*
 * Сохраняет Episode в db.memory.episodes И публикует атом-указатель.
 * ep->id ДОЛЖЕН быть присвоен ДО вызова (через hyper_memory_new_id()).
 * Ownership: копирует немедленно, не забирает владение *ep.
 * Возвращает 0 при успехе, -1 при ошибке (залогировано через LOG_ERROR).
 */
int episode_record(HyperMemory *hmem, const Episode *ep);

/*
 * Загружает Episode по id. Возвращает MDB_SUCCESS/MDB_NOTFOUND/код ошибки LMDB.
 */
int episode_load(MDB_txn *txn, ko_id_t episode_id, Episode *out);

#endif // KNOWLEDGE_EPISODE_H
