// memory/decay.h
#ifndef SUBCONSCIOUS_DECAY_H
#define SUBCONSCIOUS_DECAY_H

#include <stdint.h>
#include <lmdb.h>
#include "storage/hyper_atom/hyper_atom.h"

typedef struct {
    float    sti_decay_factor;      // мультипликативный decay STI за цикл (0.9 = -10%)
    float    valence_regression;    // скорость возврата valence к 0.0 (0.05 = 5% сдвига к нейтрали)
    float    truth_conf_decay;      // мультипликативный decay truth_confidence
    float    sti_archive_threshold; // порог "холодного" STI
    float    lti_archive_threshold; // порог "холодного" LTI
    float    utility_archive_threshold; // ниже этого utility атом кандидат в архив
    uint32_t batch_size;            // сколько атомов сканировать за один вызов (bounded cycle)
} DecayPolicy;

extern const DecayPolicy DECAY_POLICY_DEFAULT;

typedef struct {
    uint32_t scanned;
    uint32_t updated;
    uint32_t archived;
} DecayStats;

/*
 * Один цикл гомеостаза. ДОЛЖЕН вызываться внутри write-транзакции LMDB,
 * принадлежащей потоку db_writer (см. TASK 3 / db_writer.h::DbWriteFn).
 * txn обязан указывать на эту транзакцию перед вызовом.
 */
int subconscious_decay_cycle(MDB_txn *txn, HyperMemory *hmem, const DecayPolicy *policy, DecayStats *out_stats);

#endif
