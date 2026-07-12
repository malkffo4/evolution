// critic.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage/db/db.h"
#include "storage/node/node.h"
#include "storage/edge/edge.h"
#include "storage/graph/graph.h"
#include "storage/string_pool/string_pool.h"
#include "math/hash.h"

void analyze_error(const char *error_log, uint64_t task_target_id, MDB_txn *txn) {
    if (!txn) return;
    printf("\n\033[31m[КРИТИК] Анализ провала для узла ID: %lu...\033[0m\n", task_target_id);

    // 1. Создаем узел отрицательного опыта (Боль/Провал)
    uint64_t fail_id = djb2_hash("FAILURE_STATE");
    Node fail_node = { .id = fail_id, .name_hash = add_string_to_pool(txn, "FAILURE_STATE")};
    create_node(txn, &fail_node);

    // 2. Создаем связь "Приводит к ошибке" от целевого узла к провалу
    Edge penalty_edge;
    penalty_edge.key.source = task_target_id;
    penalty_edge.key.target = fail_id;
    penalty_edge.key.relation = add_string_to_pool(txn, "LEADS_TO_ERROR");
    // penalty_edge.weight = 0.9f; // Высокая уверенность ИИ, что это действие сломано
    // penalty_edge.context = 0;
    // penalty_edge.occurrences = 1;

    upsert_edge(txn, &penalty_edge);

    if (error_log && strlen(error_log) > 0) {
        printf("  -> \033[90mЛог ошибки учтен. Вес путей к этому узлу будет снижен.\033[0m\n");
    } else {
        printf("  -> \033[90mСкрипт упал молча. Связь LEADS_TO_ERROR закреплена в графе.\033[0m\n");
    }
}
