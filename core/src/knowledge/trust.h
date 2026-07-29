// knowledge/trust.h
#ifndef KNOWLEDGE_TRUST_H
#define KNOWLEDGE_TRUST_H

#include <stdbool.h>

#include "types/id.h"
#include "storage/hyper_atom/hyper_atom.h"

/*
 * RFC-0001: Adaptive Planner — универсальная оценка доверия к исполняемому
 * объекту знаний (сегодня — Algorithm; в перспективе — Operator/Plan).
 *
 * Не вводит нового хранилища. Оценка доверия — это обычный NeuroAtom со
 * стабильным детерминированным id, живущий в ТОЙ ЖЕ таблице db.graph.hyper.atoms,
 * где уже лежат HAS_ALGORITHM, HAS_FLAW и весь остальной граф знаний.
 *
 * Обновление идёт через уже существующий hyper_assert() — намеренно НЕ
 * через hyper_assert_unique()/hyper_assert_with_cause(): тем нужна
 * идемпотентность факта ("такое утверждение уже есть — не пишем повторно"),
 * а доверию нужна ровно противоположная семантика — update-in-place
 * изменяющегося значения по тому же id.
 *
 * Поля NeuroAtom переиспользуются по их же документированному назначению
 * (см. Epistemic/Attentional/Teleological vector в storage/hyper_atom/hyper_atom.h):
 *
 *   truth_mean       — собственно доверие, [0..1], сглаживается EMA.
 *   truth_confidence — сколько накоплено наблюдений (растёт при любом исходе,
 *                      отдельно от того, что эти наблюдения говорят).
 *   sti / lti / utility — НЕ декоративные: связывают атом доверия с уже
 *                      существующим циклом забывания/архивации
 *                      (memory/decay.c) без единой новой строчки логики
 *                      decay. Хорошо проверенное доверие держится в памяти
 *                      прочно; едва сэмплированное — легко "остывает",
 *                      что даёт дешёвую встроенную форму exploration.
 *   args[0]           — REF(subject_id): благодаря этому
 *                      hyper_find_by_participant(hmem, subject_id, ...)
 *                      сразу видит атом доверия наравне с HAS_ALGORITHM/
 *                      HAS_FLAW — без дополнительного кода.
 */

#define TRUST_PRIOR          0.5f
// Тот же темп сглаживания, что у Edge.confidence в
// storage/graph/graph.c::upsert_edge — единая константа на всю кодобазу.
#define TRUST_LEARNING_RATE  0.2f

typedef enum {
    TRUST_DOMAIN_ALGORITHM = 1,
    // Следующий домен добавляется сюда без изменения хранения:
    // trust_atom_id() гарантирует отсутствие коллизий id между доменами.
    // Кандидат №1: TRUST_DOMAIN_OPERATOR — для runtime/planner.c,
    // как только там появится вторая конкурирующая реализация Capability.
} TrustDomain;

// Текущая оценка доверия. Если наблюдений ещё не было — возвращает
// нейтральный приор TRUST_PRIOR, ничего не создавая и не изменяя.
float trust_get(HyperMemory *hmem, TrustDomain domain, node_id_t subject_id);

// Обновляет оценку по результату одного исполнения/использования.
// Требует активную write-транзакцию в hmem->txn.
int trust_update(HyperMemory *hmem, TrustDomain domain, node_id_t subject_id, bool success);

#endif // KNOWLEDGE_TRUST_H
