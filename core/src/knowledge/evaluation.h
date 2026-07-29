// knowledge/evaluation.h
//
// ЗАМЕНЯЕТ прежний набросок trust.h/trust.c — тот использовал вычисляемый
// (XOR) id как идентичность объекта доверия. Здесь идентичность — это
// (process_id, args), как у любого другого HyperAtom, а не отдельная
// хеш-схема. trust.h/trust.c можно удалить, они не используются.
#ifndef KNOWLEDGE_EVALUATION_H
#define KNOWLEDGE_EVALUATION_H

#include <stdbool.h>

#include "types/id.h"
#include "storage/hyper_atom/hyper_atom.h"

/*
 * RFC-0001: Adaptive Planner (v2) — универсальная оценка когнитивных
 * объектов: алгоритмов, навыков, правил, гипотез, предсказаний.
 * Planner — первый потребитель, не единственный.
 *
 * Ничего нового не хранится: Evaluation и Score — обычные NeuroAtom
 * в db.graph.hyper.atoms, идентифицируются через process_id + args,
 * как HAS_ALGORITHM/HAS_FLAW уже сегодня. Индексация (idx_process,
 * idx_args, idx_context, idx_causal) — целиком существующая.
 *
 * ── Evaluation (наблюдение) ──────────────────────────────────────────
 * Неизменяемый факт: "субъект X в момент T дал исход O". Своя запись на
 * каждое наблюдение (process = OBSERVED_OUTCOME, PROC_KIND_EVENT).
 * Пишется через hyper_assert() напрямую (НЕ hyper_assert_unique/
 * _with_cause — той нужна идемпотентность факта, а повторные по форме
 * наблюдения — это не дубликаты, а разные события).
 * Причинность (какой эпизод породил наблюдение) — тот же idx_causal,
 * которым уже пользуются OP_ASSERT/OP_DERIVE и critic_ops.c.
 *
 * ── Score (свёртка) ──────────────────────────────────────────────────
 * Единственный мутируемый rollup-атом на (domain, subject)
 * (process = HAS_SCORE, PROC_KIND_RELATION), находится через
 * hyper_find_by_participant(subject_id) + фильтр по process+domain —
 * без вычисляемого id, без сканов по всей базе.
 *
 *   truth_mean       — сама оценка, [0..1].
 *   truth_confidence — растёт с числом наблюдений (0 -> 1 асимптотически).
 *   sti/lti/utility  — переиспользуются как есть: подключают Score к уже
 *                     существующему циклу decay/archive без единой новой
 *                     строчки логики забывания; Evaluation-атомы
 *                     намеренно "холоднее" (lti мало) — они материал для
 *                     score_recompute(), а не самостоятельные убеждения.
 *
 * Два пути получить/обновить Score:
 *   score_update()    — дёшево, инкрементальный EMA-шаг. Для горячего
 *                        пути (Planner вызывает после каждого исполнения).
 *   score_recompute()  — сворачивает ВСЮ историю Evaluation заново.
 *                        Это и есть "пересмотр убеждений": формулу
 *                        свёртки можно менять, не трогая ни хранилище,
 *                        ни identity-модель, ни потребителей score_get().
 */

#define SCORE_PRIOR           0.5f
// Тот же темп сглаживания, что у Edge.confidence в
// storage/graph/graph.c::upsert_edge — единая константа на кодобазу.
#define SCORE_LEARNING_RATE   0.2f

typedef enum {
    COGNITIVE_DOMAIN_ALGORITHM  = 1,
    COGNITIVE_DOMAIN_SKILL      = 2,
    COGNITIVE_DOMAIN_RULE       = 3,
    COGNITIVE_DOMAIN_HYPOTHESIS = 4,
    COGNITIVE_DOMAIN_PREDICTION = 5,
    // Новый домен = новое значение тега в args[1], без изменения схемы
    // хранения. Следующий естественный потребитель — runtime/planner.c
    // (выбор Native-реализации Capability), как только там появится
    // вторая конкурирующая реализация: TRUST_DOMAIN_OPERATOR.
} CognitiveDomain;

// Записывает одно неизменяемое наблюдение. outcome в [0..1] — не только
// бинарный успех/провал: для Prediction это может быть степень близости
// к факту, для Hypothesis — сила подтверждения.
// cause_id  — эпизод/факт, породивший это наблюдение (0, если нет).
// context_id — контекст (см. OP_SPAWN_CTX): 0 для базовой реальности.
// Возвращает id созданного атома или 0 при ошибке.
node_id_t evaluation_record(HyperMemory *hmem, CognitiveDomain domain,
                             node_id_t subject_id, float outcome,
                             node_id_t cause_id, node_id_t context_id);

// Читает текущую свёртку. Нет записи -> нейтральный приор SCORE_PRIOR,
// ничего не создавая.
float score_get(HyperMemory *hmem, CognitiveDomain domain, node_id_t subject_id);

// Записывает наблюдение И инкрементально обновляет свёртку одним вызовом.
// Это то, что вызывает Adaptive Planner после каждого исполнения.
int score_update(HyperMemory *hmem, CognitiveDomain domain, node_id_t subject_id,
                  float outcome, node_id_t cause_id, node_id_t context_id);

// Пересчитывает свёртку заново из ВСЕХ Evaluation-атомов субъекта —
// "пересмотр убеждений", а не инкремент. Возвращает 0 при успехе
// (в т.ч. если наблюдений нет — тогда свёртка не создаётся/не трогается).
int score_recompute(HyperMemory *hmem, CognitiveDomain domain, node_id_t subject_id);

#endif // KNOWLEDGE_EVALUATION_H
