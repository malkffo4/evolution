// tests/ontology_test.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#include <lmdb.h>

#include "runtime/vm/vm.h"
#include "runtime/vm/vm_context.h"
#include "runtime/vm/instruction.h"
#include "runtime/compiler/pipeline.h"
#include "runtime/operator/operator.h"
#include "runtime/ops/opcode.h"
#include "memory/working.h"
#include "storage/db/db.h"
#include "storage/node/node.h"
#include "storage/edge/edge.h"
#include "storage/graph/graph.h"
#include "storage/string_pool/string_pool.h"
#include "storage/property.h"
#include "math/hash.h"
#include "knowledge/knowledge_cache.h"

// WorkingMemory global_wm;
// volatile sig_atomic_t g_running = 1;

static void set_node_property(MDB_txn *txn, node_id_t node_id, const char *key, int value) {
    uint64_t prop_key = djb2_hash(key);
    struct { node_id_t nid; uint64_t hash; } db_key = { node_id, prop_key };
    MDB_val mdb_key = { sizeof(db_key), &db_key };

    NodeProperty header = { .type = PROP_INT, .size = sizeof(value) };
    size_t total = sizeof(header) + sizeof(value);
    char *buf = malloc(total);
    memcpy(buf, &header, sizeof(header));
    memcpy(buf + sizeof(header), &value, sizeof(value));

    MDB_val mdb_val = { total, buf };
    mdb_put(txn, db.graph.properties, &mdb_key, &mdb_val, 0);
    free(buf);
}

int main(void) {
    system("rm -rf ./test_onto_db");
    assert(init_lmdb("./test_onto_db") == MDB_SUCCESS);

    // ===== ФАЗА 1: Write-транзакция (создание данных) =====
    MDB_txn *txn;
    assert(mdb_txn_begin(db.env, NULL, 0, &txn) == 0);

    uint64_t id_strcpy = djb2_hash("strcpy");
    uint64_t id_memcpy = djb2_hash("memcpy");
    uint64_t id_user   = djb2_hash("UserInput");

    Node ns = { .id = id_strcpy, .name_hash = add_string_to_pool(txn, "strcpy"), .type = NODE_CONCEPT };
    Node nm = { .id = id_memcpy, .name_hash = add_string_to_pool(txn, "memcpy"), .type = NODE_CONCEPT };
    Node nu = { .id = id_user,   .name_hash = add_string_to_pool(txn, "UserInput"), .type = NODE_CONCEPT };
    assert(create_node(txn, &ns) == MDB_SUCCESS);
    assert(create_node(txn, &nm) == MDB_SUCCESS);
    assert(create_node(txn, &nu) == MDB_SUCCESS);

    // Добавляем ключ свойства в пул (write-транзакция)
    uint64_t dangerous_id = add_string_to_pool(txn, "dangerous");

    set_node_property(txn, id_strcpy, "dangerous", 1);
    set_node_property(txn, id_memcpy, "dangerous", 0);

    uint64_t rel_receives = djb2_hash("RECEIVES");
    Edge e = { .key = { id_strcpy, rel_receives, id_user }, .confidence = 1.0f, .evidence_count = 1 };
    assert(create_edge(txn, &e) == MDB_SUCCESS);
    mdb_txn_commit(txn);

    MDB_txn *check_txn;
    assert(mdb_txn_begin(db.env, NULL, MDB_RDONLY, &check_txn) == 0);
    MDB_val ck, cd;
    struct { node_id_t nid; uint64_t hash; } ck_key = { id_strcpy, djb2_hash("dangerous") };
    ck.mv_size = sizeof(ck_key);
    ck.mv_data = &ck_key;
    int rc_chk = mdb_get(check_txn, db.graph.properties, &ck, &cd);
    if (rc_chk == MDB_SUCCESS) {
        NodeProperty hdr;
        memcpy(&hdr, cd.mv_data, sizeof(hdr));
        int val;
        memcpy(&val, (char*)cd.mv_data + sizeof(hdr), sizeof(int));
        printf("Direct read after commit: node=%lu, key_hash=%lu, value=%d\n", id_strcpy, djb2_hash("dangerous"), val);
    } else {
        printf("Direct read failed: %s\n", mdb_strerror(rc_chk));
    }
    mdb_txn_abort(check_txn);

    // ===== ФАЗА 2: Read-only транзакция (Virtual Mind: предзагрузка) =====
    assert(mdb_txn_begin(db.env, NULL, MDB_RDONLY, &txn) == 0);

    VMContext ctx;
    WorkingMemory wm_stub = {0};
    assert(vm_init(&ctx, txn, &wm_stub) == VM_OK);
    operator_registry_init();

    assert(knowledge_cache_load_properties(&ctx, txn, id_strcpy) == MDB_SUCCESS);
    assert(knowledge_cache_load_properties(&ctx, txn, id_memcpy) == MDB_SUCCESS);

    // Получаем указатель на строку "dangerous" из пула
    const char *dangerous_str = get_string_from_pool(txn, dangerous_id);
    assert(dangerous_str != NULL);

    // ===== ФАЗА 3: Запуск VM с изолированным кэшем =====
    Instruction algo_code[] = {
        { .operator_id = OP_PROP_GET, .arg[0] = 1, .arg[1] = 1, .arg[2] = 0 },
        { .operator_id = OP_SET_TMP, .arg[0] = 0, .arg[1] = 1 },
        { .operator_id = OP_HALT }
    };
    Pipeline algo_pipeline = { .code = algo_code, .code_len = 3, .capacity = 3 };
    algo_pipeline.constants.int_consts = NULL;
    algo_pipeline.constants.int_count = 0;

    // Регистр R0 = ключ "dangerous"
    ctx.reg[0].type = REG_STRING;
    ctx.reg[0].string.data = dangerous_str;
    ctx.reg[0].string.len  = (uint32_t)strlen(dangerous_str);

    // Тест 1: strcpy
    ctx.reg[1].type = REG_NODE; ctx.reg[1].node = id_strcpy;
    int rc = vm_execute(&ctx, &algo_pipeline);
    assert(rc == VM_OK);
    int64_t strcpy_risk = ctx.scratchpad[0].value;
    assert(strcpy_risk == 1);

    // Тест 2: memcpy
    ctx.reg[1].type = REG_NODE; ctx.reg[1].node = id_memcpy;
    memset(ctx.scratchpad, 0, sizeof(ctx.scratchpad));
    rc = vm_execute(&ctx, &algo_pipeline);
    assert(rc == VM_OK);
    int64_t memcpy_risk = ctx.scratchpad[0].value;
    assert(memcpy_risk == 0);

    printf("Ontology test passed: strcpy risk=%ld, memcpy risk=%ld\n", (long)strcpy_risk, (long)memcpy_risk);

    vm_destroy(&ctx);
    mdb_txn_abort(txn);
    close_lmdb();
    system("rm -rf ./test_onto_db");
    return 0;
}
