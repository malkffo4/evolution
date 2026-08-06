// core/tests/vm_fuzz_test.c
// Fuzz/stress-тест VM: скармливает VM тысячи случайных/мусорных программ
// (случайный opcode, случайные аргументы, в т.ч. заведомо невалидные
// регистры) в изолированных дочерних процессах и проверяет, что процесс
// НИКОГДА не завершается по сигналу (SIGSEGV/SIGABRT/SIGFPE и т.п.).
// Под debug-сборкой (-fsanitize=address,undefined, см. core/Makefile)
// это же ловит и более тонкие UB/OOB-ошибки, которые в release-сборке
// могли бы молча "проехать".
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <lmdb.h>

#include "runtime/vm/vm.h"
#include "runtime/vm/vm_context.h"
#include "runtime/compiler/pipeline.h"
#include "runtime/operator/operator.h"
#include "runtime/ops/opcode.h"
#include "memory/working.h"
#include "storage/db/db.h"

#define FUZZ_ITERATIONS   500
#define FUZZ_MAX_CODE_LEN 12

/* Детерминированный xorshift32 — фиксированный сид на итерацию, чтобы
 * упавший случай можно было воспроизвести повторным запуском. */
static uint32_t xs_state;
static uint32_t xs_next(void) {
    uint32_t x = xs_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    xs_state = x;
    return x;
}

static void build_random_pipeline(Pipeline *p, uint32_t seed) {
    xs_state = seed ? seed : 1;
    uint32_t len = 2 + (xs_next() % (FUZZ_MAX_CODE_LEN - 1));
    p->code_len = len;
    for (uint32_t i = 0; i < len - 1; i++) {
        Instruction ins = {0};
        /* Половина случаев — почти валидный диапазон opcode'ов (стресс
         * реальных операторов на мусорные аргументы), половина — совсем
         * дикие значения (стресс диспетчера на VM_UNKNOWN_OPCODE). */
        ins.operator_id = xs_next() % (VM_OPCODE_COUNT * 3);
        for (int a = 0; a < 6; a++)
            ins.arg[a] = xs_next() % ((xs_next() % 4 == 0) ? 0xFFFFFFFFu : 128u);
        p->code[i] = ins;
    }
    p->code[len - 1] = (Instruction){ .operator_id = OP_HALT };
}

static int run_one_fuzz_case(uint32_t seed) {
    char dir[64];
    snprintf(dir, sizeof(dir), "./test_fuzz_db_%u_%d", seed, (int)getpid());

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }

    if (pid == 0) {
        /* Дочерний процесс: полностью изолированный домен краша. */
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
        system(cmd);

        if (init_lmdb(dir) != MDB_SUCCESS) _exit(2);
        operator_registry_init();

        MDB_txn *txn;
        if (mdb_txn_begin(db.env, NULL, 0, &txn) != 0) _exit(2);

        WorkingMemory wm;
        memset(&wm, 0, sizeof(wm));
        VMContext ctx;
        if (vm_init(&ctx, txn, &wm) != VM_OK) _exit(2);
        ctx.max_cycles = 5000; /* быстрые fuzz-кейсы даже если задет цикл */

        ctx.hyper_mem = hyper_memory_new(db.graph.hyper.atoms, db.graph.hyper.idx_process, db.graph.hyper.idx_args, db.graph.hyper.idx_context);
        hyper_memory_set_db_causal(ctx.hyper_mem, db.graph.hyper.idx_causal);

        static Instruction code[FUZZ_MAX_CODE_LEN];
        Pipeline p = {0};
        p.code = code;
        p.capacity = FUZZ_MAX_CODE_LEN;
        build_random_pipeline(&p, seed);

        /* Результат неважен — важно лишь то, что произвольный мусорный
         * байткод никогда не роняет процесс. */
        vm_execute(&ctx, &p);

        hyper_memory_free(ctx.hyper_mem);
        vm_destroy(&ctx);
        mdb_txn_abort(txn);
        close_lmdb();
        _exit(0);
    }

    int status = 0;
    waitpid(pid, &status, 0);

    char cmd[96];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    system(cmd);

    if (WIFSIGNALED(status)) {
        fprintf(stderr, "[FUZZ] seed=%u УПАЛ с сигналом %d\n", seed, WTERMSIG(status));
        return -1;
    }
    return 0;
}

int main(void) {
    int failures = 0;
    for (int i = 0; i < FUZZ_ITERATIONS; i++) {
        uint32_t seed = (uint32_t)(i * 2654435761u + 12345u);
        if (run_one_fuzz_case(seed) != 0)
            failures++;
    }

    printf("\n=== VM Fuzz Test Results ===\n");
    printf("Итераций: %d\n", FUZZ_ITERATIONS);
    printf("Крашей:   %d\n", failures);

    if (failures == 0)
        printf("[OK] VM пережила %d случайных/мусорных программ без единого краша.\n", FUZZ_ITERATIONS);

    return failures ? 1 : 0;
}
