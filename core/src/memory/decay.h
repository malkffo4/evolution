// memory/decay.h
#pragma once

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

/*
 * Публичная обёртка над внутренней архивацией атома: перемещает атом в
 * холодное хранилище (dbi_archive) и убирает его из активных индексов
 * (idx_process/idx_args/idx_context/idx_causal), но НЕ трогает
 * dbi_atoms-запись других атомов. В отличие от subconscious_decay_cycle()
 * (который решает САМ, что архивировать, на основе sti/lti/utility), эта
 * функция архивирует НЕМЕДЛЕННО и БЕЗУСЛОВНО конкретный атом, переданный
 * вызывающей стороной.
 *
 * Основной потребитель: OP_MERGE_CTX (runtime/ops/hyper_ops.c) — чтобы
 * временные графовые Code-as-Data инструкции синтеза (PROC_KIND_INSTRUCTION)
 * не продвигались в базовую реальность вместе с реально выведенным знанием,
 * но при этом оставались доступны для explainability/OP_TRACE через архив,
 * а не терялись безвозвратно.
 *
 * ДОЛЖНА вызываться внутри write-транзакции db_writer, как и все прочие
 * мутации HyperMemory. Не создаёт новых объектов, не влияет на atom->id.
 */
void hyper_atom_archive(MDB_txn *txn, HyperMemory *hmem, const NeuroAtom *atom);
