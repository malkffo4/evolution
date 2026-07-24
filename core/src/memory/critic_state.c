// memory/critic_state.c
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

#include "runtime/logging/logging.h"
#include "runtime/vm/vm_status.h"

#define MAX_QUARANTINE_NODES 64
#define QUARANTINE_BASE_COOLDOWN_SEC 60 // 1 минута отдыха для зациклившегося алгоритма

typedef struct {
    uint64_t algo_id;
    int consecutive_failures;
    time_t quarantined_until;
} QuarantineEntry;

static QuarantineEntry quarantine_list[MAX_QUARANTINE_NODES];

/* -----------------------------------------------
 * Подсистема Критика (Карантин)
 * ----------------------------------------------- */
// Сброс карантина при старте
void init_quarantine(void) {
    memset(quarantine_list, 0, sizeof(quarantine_list));
}

bool is_quarantined(uint64_t algo_id) {
    time_t now = time(NULL);
    for (int i = 0; i < MAX_QUARANTINE_NODES; i++) {
        if (quarantine_list[i].algo_id == algo_id) {
            return (quarantine_list[i].quarantined_until > now);
        }
    }
    return false;
}

void record_execution_result(uint64_t algo_id, int rc) {
    time_t now = time(NULL);
    int empty_slot = -1;

    for (int i = 0; i < MAX_QUARANTINE_NODES; i++) {
        if (quarantine_list[i].algo_id == algo_id) {
            if (rc == VM_OK || rc == VM_NOT_FOUND) {
                // Успешное выполнение — сбрасываем счетчик ошибок
                quarantine_list[i].consecutive_failures = 0;
                quarantine_list[i].quarantined_until = 0;
            } else {
                quarantine_list[i].consecutive_failures++;
                if (quarantine_list[i].consecutive_failures >= 3) {
                    // Экспоненциальный бэкофф: 60с -> 120с -> 240с...
                    int multiplier = 1 << (quarantine_list[i].consecutive_failures - 3);
                    quarantine_list[i].quarantined_until = now + (QUARANTINE_BASE_COOLDOWN_SEC * multiplier);
                    LOG_ERROR("[CRITIC] Algorithm %llu quarantined for %d sec (Error: %d). Loop detected.",
                              (unsigned long long)algo_id, QUARANTINE_BASE_COOLDOWN_SEC * multiplier, rc);
                }
            }
            return;
        }
        if (quarantine_list[i].algo_id == 0 && empty_slot == -1) {
            empty_slot = i;
        }
    }

    // Добавляем новую запись, если произошла ошибка
    if ((rc != VM_OK && rc != VM_NOT_FOUND) && empty_slot != -1) {
        quarantine_list[empty_slot].algo_id = algo_id;
        quarantine_list[empty_slot].consecutive_failures = 1;
        quarantine_list[empty_slot].quarantined_until = 0;
    }
}
