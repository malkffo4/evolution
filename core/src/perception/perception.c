// perception/perception.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <cjson/cJSON.h>

#include "core/globals.h"
#include "memory/working.h"
#include "storage/db/db.h"
#include "storage/graph/graph.h"
#include "storage/string_pool/string_pool.h"
#include "storage/hyper_atom/hyper_atom.h"
#include "storage/property/property.h"
#include "math/hash.h"
#include "runtime/logging/logging.h"
#include "runtime/operator/operator.h"
#include "runtime/ops/graph_encoding.h"   // graph_pack_args()
#include "knowledge/claim_validator.h"
#include "knowledge/event_queue.h"

static float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

int perceive_and_activate(const char *json_str, WorkingMemory *wm, MDB_txn *txn, HyperMemory *hmem) {
    cJSON *json = cJSON_Parse(json_str);
    if (json == NULL) return -1;

    // --- Обработка старого формата Nodes -> переводим в HyperAtom ---
    cJSON *nodes = cJSON_GetObjectItemCaseSensitive(json, "nodes");
    cJSON *node = NULL;
    if (cJSON_IsArray(nodes)) {
        cJSON_ArrayForEach(node, nodes) {
            cJSON *id = cJSON_GetObjectItemCaseSensitive(node, "id");
            cJSON *label = cJSON_GetObjectItemCaseSensitive(node, "label");
            cJSON *danger_json = cJSON_GetObjectItemCaseSensitive(node, "danger");
            cJSON *utility_json = cJSON_GetObjectItemCaseSensitive(node, "utility");

            float danger = cJSON_IsNumber(danger_json) ? (float)danger_json->valuedouble : 0.1f;
            float utility = cJSON_IsNumber(utility_json) ? (float)utility_json->valuedouble : 0.1f;

            if (cJSON_IsString(id) && cJSON_IsString(label)) {
                uint64_t node_id = djb2_hash(id->valuestring);

                // Добавляем в строковый пул, чтобы retrieve работал
                add_string_to_pool(txn, label->valuestring);
                add_string_to_pool(txn, id->valuestring);

                // Создаем сущность в Гиперграфе вместо legacy-узла
                NeuroAtom concept = {0};
                concept.id = node_id;
                concept.process_id = proc_make(djb2_hash("CONCEPT"), PROC_KIND_ENTITY);
                concept.args[0].raw = HYPER_MAKE_REF(node_id);
                concept.truth_mean = 1.0f;
                concept.truth_confidence = 1.0f;
                concept.sti = 0.5f;
                hyper_assert_unique(txn, hmem, &concept);

                // Вызываем активацию (она сама берет блокировку внутри)
                wm_activate(wm, node_id, 1.0f, danger);

                // Явно блокируем WM перед ручной итерацией
                wm_wrlock(wm);
                for (uint32_t i = 0; i < wm->count; i++) {
                    if (wm->nodes[i].node_id == node_id) {
                        wm->nodes[i].state.danger = danger;
                        wm->nodes[i].state.usefulness = utility;
                        break;
                    }
                }
                wm_unlock(wm);

                int64_t zero_cooldown = 0;
                property_set(txn, node_id, "cooldown_until", PROP_INT, &zero_cooldown, sizeof(zero_cooldown));
                property_set(txn, node_id, "label", PROP_STRING, label->valuestring, strlen(label->valuestring)+1);
            }
        }
    }

    // --- Обработка старого формата Edges -> переводим в HyperAtom ---
    cJSON *edges = cJSON_GetObjectItemCaseSensitive(json, "edges");
    cJSON *edge = NULL;
    if (cJSON_IsArray(edges)) {
        cJSON_ArrayForEach(edge, edges) {
            cJSON *source = cJSON_GetObjectItemCaseSensitive(edge, "source");
            cJSON *target = cJSON_GetObjectItemCaseSensitive(edge, "target");
            cJSON *relation = cJSON_GetObjectItemCaseSensitive(edge, "relation");

            if (cJSON_IsString(source) && cJSON_IsString(target) && cJSON_IsString(relation)) {
                add_string_to_pool(txn, relation->valuestring);
                add_string_to_pool(txn, source->valuestring);
                add_string_to_pool(txn, target->valuestring);

                NeuroAtom fwd = {0};
                fwd.id = hyper_memory_new_id(hmem);
                fwd.process_id = proc_make(djb2_hash(relation->valuestring), PROC_KIND_RELATION);
                fwd.args[0].raw = HYPER_MAKE_REF(djb2_hash(source->valuestring));
                fwd.args[1].raw = HYPER_MAKE_REF(djb2_hash(target->valuestring));
                fwd.truth_mean = 1.0f;
                fwd.truth_confidence = 0.5f;
                fwd.sti = 0.5f;
                fwd.lti = 0.1f;
                hyper_assert_unique(txn, hmem, &fwd);
            }
        }
    }

    cJSON_Delete(json);
    return 0;
}

static ko_id_t resolve_arg(MDB_txn *txn, cJSON *arg_item) {
    if (cJSON_IsString(arg_item)) {
        const char *str = arg_item->valuestring;
        char *end;
        long long ival = strtoll(str, &end, 10);
        if (*end == '\0') return (ko_id_t)(ival) | HYPER_TYPE_INT;
        double dval = strtod(str, &end);
        if (*end == '\0') {
            union { double d; ko_id_t i; } u;
            u.d = dval;
            return u.i | HYPER_TYPE_FLOAT;
        }
        // Обязательно сохраняем строку в пул, иначе retrieve её потеряет!
        add_string_to_pool(txn, str);
        return HYPER_MAKE_REF(djb2_hash(str));
    } else if (cJSON_IsNumber(arg_item)) {
        double num = arg_item->valuedouble;
        if (num == (long long)num) {
            return (ko_id_t)((long long)num) | HYPER_TYPE_INT;
        } else {
            union { double d; ko_id_t i; } u;
            u.d = num;
            return u.i | HYPER_TYPE_FLOAT;
        }
    } else if (cJSON_IsBool(arg_item)) {
        return (ko_id_t)(uint64_t)(arg_item->valueint ? 1 : 0) | HYPER_TYPE_INT;
    }
    return 0;
}

static bool is_ordering_relation(ko_id_t process_id) {
    static ko_id_t precedes_full = 0;
    if (!precedes_full) precedes_full = proc_make(djb2_hash("PRECEDES"), PROC_KIND_RELATION);
    return process_id == precedes_full;
}

int perceive_hyper_json(const char *json_str, MDB_txn *txn, HyperMemory *hmem) {
    if (!json_str || !hmem) return -1;

    cJSON *root = cJSON_Parse(json_str);
    if (!root) { LOG_ERROR("perceive_hyper_json: bad JSON"); return -1; }

    cJSON *atoms = cJSON_GetObjectItem(root, "atoms");
    if (!cJSON_IsArray(atoms)) { cJSON_Delete(root); return -1; }

    cJSON *atom_item;

    cJSON_ArrayForEach(atom_item, atoms) {
        cJSON *kind_probe = cJSON_GetObjectItem(atom_item, "kind");
        if (cJSON_IsString(kind_probe) && strcmp(kind_probe->valuestring, "instruction") == 0) {
            // Graph-native Code-as-Data atom (см. TODO.md Шаг B, docs/10_VM.md).
            // Не имеет "process" в человекочитаемом смысле: process_id
            // напрямую кодирует OperatorID, точно как OP_ASSERT_INSTRUCTION
            // строит его в runtime/ops/graph_ops.c. Это единственный путь,
            // которым Python (knowledge_compiler.py) может синтезировать
            // исполняемые графовые программы, минуя VM целиком — VM их
            // только ИНТЕРПРЕТИРУЕТ позже через OP_EVAL_GRAPH.
            cJSON *opcode_json = cJSON_GetObjectItem(atom_item, "opcode");
            cJSON *fields_json = cJSON_GetObjectItem(atom_item, "fields");
            if (!cJSON_IsNumber(opcode_json) || !cJSON_IsArray(fields_json)) {
                LOG_WARN("perceive_hyper_json: instruction atom missing 'opcode'/'fields', skipped");
                continue;
            }

            uint32_t fields[6] = {0};
            int fcount = cJSON_GetArraySize(fields_json);
            for (int fi = 0; fi < fcount && fi < 6; fi++) {
                cJSON *fe = cJSON_GetArrayItem(fields_json, fi);
                uint32_t v = cJSON_IsNumber(fe) ? (uint32_t)fe->valuedouble : 0;
                fields[fi] = v & GRAPH_INSTR_FIELD_MASK;   // жёсткий клэмп до 10 бит, входу не доверяем
            }
            uint64_t packed = graph_pack_args(fields);

            NeuroAtom instr = {0};
            instr.process_id  = proc_make((ko_id_t)opcode_json->valuedouble, PROC_KIND_INSTRUCTION);
            instr.args[0].raw = (ko_id_t)(packed & HYPER_VALUE_MASK) | HYPER_TYPE_INT;

            cJSON *wide_json = cJSON_GetObjectItem(atom_item, "wide");
            if (wide_json) instr.args[1].raw = resolve_arg(txn, wide_json);

            cJSON *truth_json2 = cJSON_GetObjectItem(atom_item, "truth");
            if (cJSON_IsObject(truth_json2)) {
                cJSON *m = cJSON_GetObjectItem(truth_json2, "mean");
                cJSON *c = cJSON_GetObjectItem(truth_json2, "confidence");
                instr.truth_mean       = cJSON_IsNumber(m) ? clampf((float)m->valuedouble, 0.f, 1.f) : 1.0f;
                instr.truth_confidence = cJSON_IsNumber(c) ? clampf((float)c->valuedouble, 0.f, 1.f) : 0.5f;
            } else {
                instr.truth_mean = 1.0f;
                instr.truth_confidence = 0.5f;   // свежая гипотеза кода, ещё не проверена исполнением
            }
            instr.sti = 0.4f;
            instr.lti = 0.05f;   // синтезированный код дёшево забывается, если ни разу не исполнен (Principle 11)

            cJSON *ctx_json2 = cJSON_GetObjectItem(atom_item, "context");
            instr.context_or_time_link = ctx_json2 ? (ko_id_t)cJSON_GetNumberValue(ctx_json2) : 0;

            cJSON *id_json2 = cJSON_GetObjectItem(atom_item, "id");
            if (cJSON_IsString(id_json2)) {
                instr.id = djb2_hash(id_json2->valuestring);
                add_string_to_pool(txn, id_json2->valuestring);
            } else {
                instr.id = hyper_memory_new_id(hmem);
            }

            ko_id_t cause_id2 = 0;
            cJSON *cause_json2 = cJSON_GetObjectItem(atom_item, "cause");
            if (cJSON_IsString(cause_json2)) {
                cause_id2 = djb2_hash(cause_json2->valuestring);
                add_string_to_pool(txn, cause_json2->valuestring);
            } else if (cJSON_IsNumber(cause_json2)) {
                cause_id2 = (ko_id_t)cJSON_GetNumberValue(cause_json2);
            }

            // PROC_KIND_INSTRUCTION уже исключён из дедупликации в
            // hyper_atom_exists() (storage/hyper_atom/hyper_atom.c) — каждая
            // инструкция уникальна по построению, как и у OP_ASSERT_INSTRUCTION.
            if (hyper_assert_with_cause(txn, hmem, &instr, cause_id2) < 0) {
                LOG_ERROR("perceive_hyper_json: failed to assert instruction atom (opcode=%.0f)",
                          opcode_json->valuedouble);
            }
            continue;   // инструкции не проходят через общую ветку relation/entity ниже
        }
        cJSON *process_json = cJSON_GetObjectItem(atom_item, "process");
        if (!cJSON_IsString(process_json)) continue;

        // КРИТИЧЕСКИ ВАЖНО: сохраняем процесс в пул, чтобы retrieve не выдавал "UNKNOWN"
        add_string_to_pool(txn, process_json->valuestring);

        ProcKind kind = PROC_KIND_ENTITY;
        cJSON *kind_json = cJSON_GetObjectItem(atom_item, "kind");
        if (cJSON_IsString(kind_json)) {
            const char *k = kind_json->valuestring;
            add_string_to_pool(txn, k);
            if      (!strcmp(k, "relation")) kind = PROC_KIND_RELATION;
            else if (!strcmp(k, "entity"))   kind = PROC_KIND_ENTITY;
            else if (!strcmp(k, "concept"))  kind = PROC_KIND_ENTITY;
            else if (!strcmp(k, "rule"))     kind = PROC_KIND_RULE;
            else if (!strcmp(k, "goal"))     kind = PROC_KIND_GOAL;
            else if (!strcmp(k, "event"))    kind = PROC_KIND_EVENT;
        }

        NeuroAtom atom = {0};
        atom.process_id = proc_make(djb2_hash(process_json->valuestring), kind);

        cJSON *args_json = cJSON_GetObjectItem(atom_item, "args");
        if (cJSON_IsArray(args_json)) {
            int n = cJSON_GetArraySize(args_json);
            for (int i = 0; i < n && i < HYPER_VAL_SLOTS; i++) {
                atom.args[i].raw = resolve_arg(txn, cJSON_GetArrayItem(args_json, i));
            }
        }

        cJSON *truth_json = cJSON_GetObjectItem(atom_item, "truth");
        if (cJSON_IsObject(truth_json)) {
            cJSON *m = cJSON_GetObjectItem(truth_json, "mean");
            cJSON *c = cJSON_GetObjectItem(truth_json, "confidence");
            atom.truth_mean       = cJSON_IsNumber(m) ? clampf((float)m->valuedouble, 0.f, 1.f) : 1.0f;
            atom.truth_confidence = cJSON_IsNumber(c) ? clampf((float)c->valuedouble, 0.f, 1.f) : 0.5f;
        } else {
            atom.truth_mean = 1.0f;
            atom.truth_confidence = 0.5f;
        }

        cJSON *attn_json = cJSON_GetObjectItem(atom_item, "attention");
        if (cJSON_IsObject(attn_json)) {
            cJSON *sti = cJSON_GetObjectItem(attn_json, "sti");
            cJSON *lti = cJSON_GetObjectItem(attn_json, "lti");
            atom.sti = cJSON_IsNumber(sti) ? (float)sti->valuedouble : 0.5f;
            atom.lti = cJSON_IsNumber(lti) ? clampf((float)lti->valuedouble, 0.f, 1.f) : 0.1f;
        } else {
            atom.sti = 0.5f;
            atom.lti = 0.1f;
        }

        cJSON *util_json = cJSON_GetObjectItem(atom_item, "utility");
        cJSON *val_json  = cJSON_GetObjectItem(atom_item, "valence");
        atom.utility = cJSON_IsNumber(util_json) ? clampf((float)util_json->valuedouble, 0.f, 1.f) : 0.0f;
        atom.valence = cJSON_IsNumber(val_json)  ? clampf((float)val_json->valuedouble, -1.f, 1.f) : 0.0f;

        cJSON *ctx_json = cJSON_GetObjectItem(atom_item, "context");
        atom.context_or_time_link = ctx_json ? (ko_id_t)cJSON_GetNumberValue(ctx_json) : 0;

        cJSON *id_json = cJSON_GetObjectItem(atom_item, "id");
        if (cJSON_IsString(id_json)) {
            atom.id = djb2_hash(id_json->valuestring);
            add_string_to_pool(txn, id_json->valuestring);
        } else {
            atom.id = hyper_memory_new_id(hmem);
        }

        int result;
        if (is_ordering_relation(atom.process_id)) {
            ko_id_t cause = 0; /* из cause_json, уже распарсенного ниже — см. существующий блок */
            node_id_t conflict = 0;
            ClaimVerdict v = claim_validate_and_assert(txn, hmem, &atom, cause,
                                                        proc_make(djb2_hash("PRECEDES"), PROC_KIND_RELATION),
                                                        0, &conflict);
            if (v == CLAIM_REJECTED_CYCLE) continue; // атом НЕ попадает в граф
            result = 0;
        } else {
            result = hyper_assert_unique(txn, hmem, &atom);
        }
        if (result != 0 && result != 1) continue;

        cJSON *props_json = cJSON_GetObjectItem(atom_item, "properties");
        if (cJSON_IsObject(props_json)) {
            cJSON *prop;
            cJSON_ArrayForEach(prop, props_json) {
                const char *pkey = prop->string;
                if (!pkey || !pkey[0]) continue;

                if (cJSON_IsString(prop) && prop->valuestring) {
                    property_set(txn, atom.id, pkey, PROP_STRING, prop->valuestring, (uint32_t)strlen(prop->valuestring) + 1);
                } else if (cJSON_IsBool(prop)) {
                    bool v = cJSON_IsTrue(prop);
                    property_set(txn, atom.id, pkey, PROP_BOOL, &v, sizeof(v));
                } else if (cJSON_IsNumber(prop)) {
                    double d = prop->valuedouble;
                    if (d == (double)(int64_t)d) {
                        int64_t v = (int64_t)d;
                        property_set(txn, atom.id, pkey, PROP_INT, &v, sizeof(v));
                    } else {
                        float v = (float)d;
                        property_set(txn, atom.id, pkey, PROP_FLOAT, &v, sizeof(v));
                    }
                } else if (cJSON_IsArray(prop) || cJSON_IsObject(prop)) {
                    char *sub = cJSON_PrintUnformatted(prop);
                    if (sub) {
                        property_set(txn, atom.id, pkey, PROP_STRING, sub, (uint32_t)strlen(sub) + 1);
                        free(sub);
                    }
                }
            }
        }

        if (cJSON_IsString(kind_json)) {
            const char *k = kind_json->valuestring;
            if (strcmp(k, "relation") && strcmp(k, "entity") && strcmp(k, "concept") &&
                strcmp(k, "rule") && strcmp(k, "goal") && strcmp(k, "event")) {
                NeuroAtom isa_atom = {0};
                isa_atom.id = hyper_memory_new_id(hmem);
                isa_atom.process_id = proc_make(djb2_hash("IS_A"), PROC_KIND_RELATION);
                isa_atom.args[0].raw = HYPER_MAKE_REF(atom.id);
                isa_atom.args[1].raw = HYPER_MAKE_REF(djb2_hash(k));
                isa_atom.truth_mean = 1.0f;
                isa_atom.truth_confidence = 1.0f;
                hyper_assert_unique(txn, hmem, &isa_atom);
            }
        }

        cJSON *cause_json = cJSON_GetObjectItem(atom_item, "cause");
        if (cause_json) {
            ko_id_t cause_id = 0;
            if (cJSON_IsString(cause_json)) {
                cause_id = djb2_hash(cause_json->valuestring);
                add_string_to_pool(txn, cause_json->valuestring);
            } else if (cJSON_IsNumber(cause_json)) {
                cause_id = (ko_id_t)cJSON_GetNumberValue(cause_json);
            }

            if (cause_id) {
                MDB_val k = { sizeof(ko_id_t), &atom.id };
                MDB_val v = { sizeof(ko_id_t), &cause_id };
                mdb_put(txn, hmem->dbi_idx_causal, &k, &v, 0);

                // Записываем в обратный индекс для InductiveExtractor
                MDB_val k_rev = { sizeof(ko_id_t), &cause_id };
                MDB_val v_rev = { sizeof(ko_id_t), &atom.id };
                mdb_put(txn, db.graph.hyper.idx_causal_rev, &k_rev, &v_rev, 0);
            }
        }

        cJSON *enqueue_json = cJSON_GetObjectItem(atom_item, "enqueue");
        if (cJSON_IsString(enqueue_json)) {
            ko_id_t queue_id = djb2_hash(enqueue_json->valuestring);
            add_string_to_pool(txn, enqueue_json->valuestring);
            event_queue_push(txn, hmem, queue_id, atom.id);
        }

        cJSON *embed_json = cJSON_GetObjectItem(atom_item, "embedding");
        if (cJSON_IsArray(embed_json) && cJSON_GetArraySize(embed_json) == VECTOR_DIM) {
            Vector128 vec;
            for (int d = 0; d < VECTOR_DIM; d++) {
                vec.data[d] = (float)cJSON_GetNumberValue(cJSON_GetArrayItem(embed_json, d));
            }
            hyper_vector_save(txn, db.graph.hyper.idx_vectors, atom.id, &vec);
        }

        if (proc_kind(atom.process_id) == PROC_KIND_GOAL) {
            wm_activate(&global_wm, atom.id, atom.sti, atom.valence);
        }
    }

    cJSON_Delete(root);
    return 0;
}
