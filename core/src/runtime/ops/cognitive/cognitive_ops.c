// runtime/ops/cognitive/cognitive_ops.c
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "math/hash.h"
#include "memory/working.h"
#include "runtime/logging/logging.h"
#include "runtime/object/object.h"
#include "runtime/register/register.h"
#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_status.h"
#include "storage/db/db.h"
#include "storage/edge/edge.h"
#include "storage/graph/graph.h"
#include "storage/node/node.h"
#include "storage/property.h"
#include "storage/string_pool/string_pool.h"
#include "storage/vector_store/vector_store.h"

// TODO
// OP_INFER: Запускает движок логического вывода для указанного Объекта Знаний.
// OP_FORK_CTX: Создает изолированную песочницу в оперативной памяти (arena.c) для генерации гипотезы.
// OP_MERGE_CTX: Если гипотеза подтвердилась как полезная, извлеченные выводы интегрируются обратно в основную графовую базу.
// OP_ANALYZE_PATH: Запускает поиск кратчайшего логического пути между двумя Объектами Знаний с учетом онтологических ограничений.

/* STREAMING_CHUNK:Configuring system headers and checking VM registers... */

// Вспомогательная функция для проверки валидности регистров
static bool check_registers(uint32_t r1, uint32_t r2) {
  return (r1 < VM_MAX_REGISTERS && r2 < VM_MAX_REGISTERS);
}

/* STREAMING_CHUNK:Implementing atomic vector similarity operator... */

/* OP_VECTOR_SIM
 * Аргументы инструкции:
 * arg[0] - Регистр-приемник (REG_FLOAT) - результат близости
 * arg[1] - Регистр первого узла (REG_NODE)
 * arg[2] - Регистр второго узла (REG_NODE)
 *
 * Атомарная операция вычисления косинусного сходства векторов.
 * Если векторы отсутствуют, делает фолбэк на SimHash (расстояние Хэмминга).
 * Это базовый вычислительный примитив для любых семантических сравнений. */
int vm_op_vector_sim(VMContext *ctx, const Instruction *ins) {
  uint32_t dst = ins->arg[0];
  uint32_t reg_a = ins->arg[1];
  uint32_t reg_b = ins->arg[2];

  if (!check_registers(dst, reg_a) || reg_b >= VM_MAX_REGISTERS) {
    LOG_ERROR("VM Engine: Invalid registers in OP_VECTOR_SIM");
    return VM_INVALID_REGISTER;
  }

  if (ctx->reg[reg_a].type != REG_NODE || ctx->reg[reg_b].type != REG_NODE) {
    LOG_WARN("VM Engine: OP_VECTOR_SIM requires node registers as input");
    return VM_INVALID_TYPE;
  }

  node_id_t node_a = ctx->reg[reg_a].node;
  node_id_t node_b = ctx->reg[reg_b].node;

  MDB_txn *txn = ctx->memory.txn;
  if (!txn) {
    LOG_ERROR("VM Engine: Active DB transaction is NULL");
    return VM_ERROR;
  }

  float emb_a[EMBEDDING_DIM];
  float emb_b[EMBEDDING_DIM];
  float similarity = 0.0f;

  // Пытаемся загрузить полные векторы для точного косинусного сходства
  if (load_embedding(txn, node_a, emb_a) == 0 &&
      load_embedding(txn, node_b, emb_b) == 0) {
    float dot = 0.0f, norm_a = 0.0f, norm_b = 0.0f;
    for (int i = 0; i < EMBEDDING_DIM; i++) {
      dot += emb_a[i] * emb_b[i];
      norm_a += emb_a[i] * emb_a[i];
      norm_b += emb_b[i] * emb_b[i];
    }
    if (norm_a > 0.0f && norm_b > 0.0f) {
      similarity = dot / (sqrtf(norm_a) * sqrtf(norm_b));
    }
  } else {
    // Фолбэк на SimHash (аппаратная замена для экономии памяти)
    Node n_a, n_b;
    if (get_node(txn, node_a, &n_a) == MDB_SUCCESS &&
        get_node(txn, node_b, &n_b) == MDB_SUCCESS) {
      uint64_t diff = n_a.simhash ^ n_b.simhash;
      int dist = __builtin_popcountll(diff);
      similarity = 1.0f - ((float)dist / 128.0f);
      if (similarity < 0.0f)
        similarity = 0.0f;
    }
  }

  vm_register_set_float(ctx, &ctx->reg[dst], (double)similarity);
  return VM_OK;
}

/* STREAMING_CHUNK:Implementing atomic node property operations... */

/* OP_PROP_GET
 * Аргументы инструкции:
 * arg[0] - Регистр-приемник значения свойства (REG_FLOAT / REG_STRING /
 * REG_INT) arg[1] - Регистр узла/связи (REG_NODE) arg[2] - Регистр-ключ имени
 * свойства (REG_STRING) Читает динамическое свойство объекта из LMDB. Позволяет
 * когнитивной программе извлекать метаданные (например, веса нейронов,
 * опасность портов, признаки котиков). */
int vm_op_prop_get(VMContext *ctx, const Instruction *ins) {
    uint32_t dst = ins->arg[0];
    uint32_t entity_reg = ins->arg[1];
    uint32_t key_reg = ins->arg[2];

    if (!check_registers(dst, entity_reg) || key_reg >= VM_MAX_REGISTERS)
        return VM_INVALID_REGISTER;

    if (ctx->reg[entity_reg].type != REG_NODE || ctx->reg[key_reg].type != REG_STRING)
        return VM_INVALID_TYPE;

    node_id_t node_id = ctx->reg[entity_reg].node;
    uint64_t prop_key = djb2_hash(ctx->reg[key_reg].string.data);

    // Ищем свойство в кэше
    for (uint32_t i = 0; i < ctx->preloaded_property_count; i++) {
        if (ctx->preloaded_properties[i].node_id == node_id &&
            ctx->preloaded_properties[i].key_hash == prop_key) {
            CachedProperty *cp = &ctx->preloaded_properties[i];
            vm_register_clear(ctx, &ctx->reg[dst]);
            switch (cp->type) {
                case PROP_INT:
                    vm_register_set_int(ctx, &ctx->reg[dst], cp->value.i);
                    break;
                case PROP_FLOAT:
                    vm_register_set_float(ctx, &ctx->reg[dst], (double)cp->value.f);
                    break;
                case PROP_BOOL:
                    vm_register_set_bool(ctx, &ctx->reg[dst], cp->value.b);
                    break;
                default:
                    return VM_INVALID_TYPE;
            }
            return VM_OK;
        }
    }
    return VM_NOT_FOUND;
}

/* OP_PROP_SET
 * Аргументы инструкции:
 * arg[0] - Регистр сущности (REG_NODE)
 * arg[1] - Регистр-ключ имени свойства (REG_STRING)
 * arg[2] - Регистр-источник значения (REG_FLOAT / REG_STRING / REG_INT)
 * Записывает динамическое свойство сущности в базу данных LMDB. */
int vm_op_prop_set(VMContext *ctx, const Instruction *ins) {
    uint32_t entity_reg = ins->arg[0];
    uint32_t key_reg = ins->arg[1];
    uint32_t val_reg = ins->arg[2];

    if (!check_registers(entity_reg, key_reg) || val_reg >= VM_MAX_REGISTERS)
        return VM_INVALID_REGISTER;

    if (ctx->reg[entity_reg].type != REG_NODE || ctx->reg[key_reg].type != REG_STRING)
        return VM_INVALID_TYPE;

    node_id_t node_id = ctx->reg[entity_reg].node;
    uint64_t prop_key = djb2_hash(ctx->reg[key_reg].string.data);
    Register *val = &ctx->reg[val_reg];

    // Ищем слот в кэше или создаём новый
    uint32_t slot = -1;
    for (uint32_t i = 0; i < ctx->preloaded_property_count; i++) {
        if (ctx->preloaded_properties[i].node_id == node_id &&
            ctx->preloaded_properties[i].key_hash == prop_key) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        if (ctx->preloaded_property_count >= MAX_PRELOADED_PROPERTIES)
            return VM_OUT_OF_MEMORY;
        slot = ctx->preloaded_property_count++;
        ctx->preloaded_properties[slot].node_id = node_id;
        ctx->preloaded_properties[slot].key_hash = prop_key;
    }

    CachedProperty *cp = &ctx->preloaded_properties[slot];
    switch (val->type) {
        case REG_INT:
            cp->type = PROP_INT;
            cp->value.i = val->i;
            break;
        case REG_FLOAT:
            cp->type = PROP_FLOAT;
            cp->value.f = (float)val->f;
            break;
        case REG_BOOL:
            cp->type = PROP_BOOL;
            cp->value.b = val->b;
            break;
        default:
            return VM_INVALID_TYPE;
    }

    // ВАЖНО: здесь нет mdb_put! Фиксация в LMDB будет через OP_COMMIT
    return VM_OK;
}

/* STREAMING_CHUNK:Implementing atomic node edge traversal operations... */

/* OP_NODE_TRAVERSE
 * Аргументы инструкции:
 * arg[0] - Регистр-приемник списка связей (REG_OBJECT / Handle OBJECT_EDGESET)
 * arg[1] - Регистр исходного узла (REG_NODE)
 * Пошаговый обход графа. Возвращает список всех исходящих связей текущего узла.
 * Позволяет внешним пайплайнам делать BFS/DFS и искать логические пути. */
int vm_op_node_traverse(VMContext *ctx, const Instruction *ins) {
  uint32_t dst = ins->arg[0];
  uint32_t src = ins->arg[1];

  if (!check_registers(dst, src))
    return VM_INVALID_REGISTER;
  if (ctx->reg[src].type != REG_NODE)
    return VM_INVALID_TYPE;

  node_id_t source_node = ctx->reg[src].node;
  MDB_txn *txn = ctx->memory.txn;

  // Аллоцируем динамический EdgeList
  EdgeList *list = calloc(1, sizeof(EdgeList));
  if (!list)
    return VM_OUT_OF_MEMORY;

  int rc = get_edges_from_node(txn, source_node, list);
  if (rc != MDB_SUCCESS) {
    free(list);
    return VM_ERROR;
  }

  // Создаем Arena-объект типа OBJECT_EDGESET
  VMHandle handle = vm_object_new(&ctx->arena, OBJECT_EDGESET);
  if (handle.index == UINT32_MAX) {
    if (list->items)
      free(list->items);
    free(list);
    return VM_OUT_OF_MEMORY;
  }

  VMObject *obj = vm_object_get(&ctx->arena, handle);
  obj->data = list;

  // Записываем дескриптор объекта в регистр-приемник
  vm_register_clear(ctx, &ctx->reg[dst]);
  ctx->reg[dst].type = REG_OBJECT;
  ctx->reg[dst].handle = handle;

  return VM_OK;
}

/* STREAMING_CHUNK:Implementing atomic working memory activation... */

/* OP_WM_ACTIVATE
 * Аргументы инструкции:
 * arg[0] - Регистр ID узла (REG_NODE)
 * arg[1] - Регистр силы возбуждения (REG_FLOAT) - уровень активации [0..1]
 * arg[2] - Регистр прайминга (REG_FLOAT) - инкремент значимости/эмоции
 * Атомарно возбуждает узел в Рабочей Памяти, выводя его в фокус внимания
 * "мозга". С помощью этой инструкции программы мышления могут "думать" о вещах,
 * привлекая к ним вычислительные ресурсы фоновых демонов подсознания. */
int vm_op_wm_activate(VMContext *ctx, const Instruction *ins) {
  uint32_t node_reg = ins->arg[0];
  uint32_t act_reg = ins->arg[1];
  uint32_t prime_reg = ins->arg[2];

  if (!check_registers(node_reg, act_reg) || prime_reg >= VM_MAX_REGISTERS) {
    return VM_INVALID_REGISTER;
  }

  if (ctx->reg[node_reg].type != REG_NODE ||
      ctx->reg[act_reg].type != REG_FLOAT ||
      ctx->reg[prime_reg].type != REG_FLOAT) {
    return VM_INVALID_TYPE;
  }

  node_id_t node_id = ctx->reg[node_reg].node;
  // float activation = (float)ctx->reg[act_reg].fval;
  // float priming = (float)ctx->reg[prime_reg].fval;

  // WorkingMemory *wm = ctx->memory.wm;
  // if (!wm) {
  //   LOG_ERROR("VM Engine: Working Memory context is missing");
  //   return VM_ERROR;
  // }

  // // Возбуждаем узел в ассоциативной памяти
  // wm_activate(wm, node_id, activation, priming);
  return VM_OK;
}

/* STREAMING_CHUNK:Implementing low-level edge write operations... */

/* OP_EDGE_WRITE
 * Аргументы инструкции:
 * arg[0] - Регистр источника (REG_NODE)
 * arg[1] - Регистр связи (REG_NODE / REG_STRING) - тип отношения
 * arg[2] - Регистр цели (REG_NODE)
 * [Регистры свойств берутся из контекста или пишутся отдельно через
 * OP_PROP_SET] Базовый примитив записи/ассоциативного связывания. Позволяет VM
 * буквально модифицировать граф знаний: "обучаться", связывать новые факты,
 * создавать гипотезы, записывать выводы. Сама логика "когда создавать гипотезу"
 * теперь описывается в пайплайне нейрона, а не жестко зашита в Си! */
int vm_op_edge_write(VMContext *ctx, const Instruction *ins) {
  uint32_t src_reg = ins->arg[0];
  uint32_t rel_reg = ins->arg[1];
  uint32_t tgt_reg = ins->arg[2];

  if (!check_registers(src_reg, rel_reg) || tgt_reg >= VM_MAX_REGISTERS) {
    return VM_INVALID_REGISTER;
  }

  if (ctx->reg[src_reg].type != REG_NODE ||
      ctx->reg[tgt_reg].type != REG_NODE) {
    return VM_INVALID_TYPE;
  }

  node_id_t source = ctx->reg[src_reg].node;
  node_id_t target = ctx->reg[tgt_reg].node;
  node_id_t relation = 0;

  MDB_txn *txn = ctx->memory.txn;

  if (ctx->reg[rel_reg].type == REG_NODE) {
    relation = ctx->reg[rel_reg].node;
  } else if (ctx->reg[rel_reg].type == REG_STRING) {
    relation = djb2_hash(ctx->reg[rel_reg].string.data);
    add_string_to_pool(txn, ctx->reg[rel_reg].string.data);
  } else {
    return VM_INVALID_TYPE;
  }

  // Формируем ребро (связь) с дефолтными физическими параметрами
  Edge edge = {
      .key = {.source = source, .relation = relation, .target = target},
      .confidence = 1.0f, // Атомарное действие по умолчанию абсолютно уверено
      .evidence_count = 1,
      .context = 0,
      .created_at = (uint64_t)time(NULL)};

  int rc = upsert_edge(txn, &edge);
  return (rc == MDB_SUCCESS) ? VM_OK : VM_ERROR;
}

/* STREAMING_CHUNK:Implementing control flow conditional branching... */

/* OP_COND_BRANCH
 * Аргументы инструкции:
 * arg[0] - Первый регистр для сравнения (REG_FLOAT / REG_INT)
 * arg[1] - Второй регистр для сравнения или константа-порог (REG_FLOAT /
 * REG_INT) arg[2] - Адрес относительного перехода (смещение IP) Базовый
 * условный переход. Позволяет пайплайну ветвиться. Например: "Если сходство
 * векторов (similarity) > 0.75, перейти к созданию связи". Это превращает
 * виртуальную когнитивную машину в Тьюринг-полный граф-автомат. */
int vm_op_cond_branch(VMContext *ctx, const Instruction *ins) {
  uint32_t reg_a = ins->arg[0];
  uint32_t reg_b = ins->arg[1];
  int32_t offset = (int32_t)ins->arg[2]; // Смещение для IP

  if (!check_registers(reg_a, reg_b))
    return VM_INVALID_REGISTER;

  bool condition_met = false;

  // Сравниваем значения в регистрах
  // if (ctx->reg[reg_a].type == REG_FLOAT && ctx->reg[reg_b].type == REG_FLOAT) {
  //   if (ctx->reg[reg_a].fval > ctx->reg[reg_b].fval) {
  //     condition_met = true;
  //   }
  // } else if (ctx->reg[reg_a].type == REG_INT &&
  //            ctx->reg[reg_b].type == REG_INT) {
  //   if (ctx->reg[reg_a].ival > ctx->reg[reg_b].ival) {
  //     condition_met = true;
  //   }
  // } else if (ctx->reg[reg_a].type == REG_FLOAT &&
  //            ctx->reg[reg_b].type == REG_INT) {
  //   if (ctx->reg[reg_a].fval > (double)ctx->reg[reg_b].ival) {
  //     condition_met = true;
  //   }
  // }

  // if (condition_met) {
  //   // Меняем Instruction Pointer в контексте VM
  //   // Минус 1 делается потому, что главный цикл VM инкрементирует IP после
  //   // выполнения инструкции
  //   ctx->ip += offset - 1;
  //   LOG_DEBUG("VM Engine: Condition met, branching by offset %d (new IP: %u)",
  //             offset, ctx->ip);
  // }

  return VM_OK;
}
