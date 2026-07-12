// #include <stdio.h>
#include <stdlib.h>
// #include <string.h>
// #include <ctype.h>
// #include "cJSON.h"

// #include "storage/semantic.h"
// #include "storage/db.h"

// // [event_123] --HAS_AGENT--> [attacker]
// // [event_123] --HAS_TECHNIQUE--> [AS_REP_roasting]
// // [event_123] --HAS_TARGET--> [user_account]
// // [event_123] --HAS_CONDITION--> [no_preauth]
// // [event_123] --HAS_RESULT--> [password_hash]

// static uint64_t build_entity_id(const char *prefix, const char *text) {
//     char buffer[256];
//     snprintf(buffer, sizeof(buffer), "%s:%s", prefix, text);
//     return djb2_hash(buffer);
// }

// static uint64_t create_semantic_node(MDB_txn *txn, const char *label, SemanticRole role) {
//     if (!txn || !label) return 0;

//     uint64_t node_id = build_entity_id("SEM", label);
//     Node node = {
//         .id = node_id,
//         .name_hash = add_string_to_pool(txn, label),
//         .semantics = (uint64_t)role
//     };
//     create_node(txn, &node);
//     return node_id;
// }

// static void connect_semantic_edge(MDB_txn *txn, uint64_t src, uint64_t tgt, const char *relation, uint64_t context) {
//     if (!txn || src == 0 || tgt == 0 || !relation) return;
//     Edge edge = {
//         .source = src,
//         .target = tgt,
//         .relation = add_string_to_pool(txn, relation),
//         .weight = 0.5f,
//         .context = (uint32_t)(context & 0xFFFFFFFF),
//         .occurrences = 1
//     };
//     upsert_edge(txn, &edge);
// }

// static bool find_connector(const char *sentence, const char **out_intent, const char **out_cause) {
//     const char *intents[] = {"want", "need", "plan", "intend", "aim", "desire", "нужно", "хочу", "планирую", "намерен", NULL};
//     const char *causes[] = {"because", "because of", "due to", "if", "when", "since", "потому что", "из-за", "если", "так как", NULL};

//     *out_intent = NULL;
//     *out_cause = NULL;

//     for (int i = 0; intents[i]; i++) {
//         if (strcasestr(sentence, intents[i])) {
//             *out_intent = intents[i];
//             break;
//         }
//     }
//     for (int i = 0; causes[i]; i++) {
//         if (strcasestr(sentence, causes[i])) {
//             *out_cause = causes[i];
//             break;
//         }
//     }
//     return (*out_intent != NULL) || (*out_cause != NULL);
// }

// int semantic_enrich_graph(MDB_txn *txn, const char *json_str) {
//     if (!txn || !json_str) return -1;

//     cJSON *json = cJSON_Parse(json_str);
//     if (!json) return -1;

//     cJSON *nodes = cJSON_GetObjectItemCaseSensitive(json, "nodes");
//     cJSON *edges = cJSON_GetObjectItemCaseSensitive(json, "edges");
//     if (!cJSON_IsArray(nodes) || !cJSON_IsArray(edges)) {
//         cJSON_Delete(json);
//         return -1;
//     }

//     int edge_index = 0;
//     cJSON *edge = NULL;
//     cJSON_ArrayForEach(edge, edges) {
//         cJSON *source = cJSON_GetObjectItemCaseSensitive(edge, "source");
//         cJSON *target = cJSON_GetObjectItemCaseSensitive(edge, "target");
//         cJSON *relation = cJSON_GetObjectItemCaseSensitive(edge, "relation");
//         if (!cJSON_IsString(source) || !cJSON_IsString(target) || !cJSON_IsString(relation)) {
//             continue;
//         }

//         char event_label[256];
//         snprintf(event_label, sizeof(event_label), "event_%d_%s_%s", edge_index++, source->valuestring, target->valuestring);
//         uint64_t event_id = create_semantic_node(txn, event_label, SEM_ROLE_EVENT);

//         uint64_t actor_id = create_semantic_node(txn, source->valuestring, SEM_ROLE_AGENT);
//         uint64_t patient_id = create_semantic_node(txn, target->valuestring, SEM_ROLE_PATIENT);
//         uint64_t action_id = create_semantic_node(txn, relation->valuestring, SEM_ROLE_ACTION);

//         connect_semantic_edge(txn, event_id, actor_id, "HAS_ACTOR", event_id);
//         connect_semantic_edge(txn, event_id, patient_id, "HAS_PATIENT", event_id);
//         connect_semantic_edge(txn, event_id, action_id, "HAS_ACTION", event_id);

//         if (strcasestr(relation->valuestring, "cause") || strcasestr(relation->valuestring, "because") || strcasestr(relation->valuestring, "leads")) {
//             uint64_t cause_id = create_semantic_node(txn, relation->valuestring, SEM_ROLE_CAUSE);
//             connect_semantic_edge(txn, cause_id, event_id, "CAUSES", event_id);
//         }
//         if (strcasestr(relation->valuestring, "want") || strcasestr(relation->valuestring, "need") || strcasestr(relation->valuestring, "plan")) {
//             uint64_t intent_id = create_semantic_node(txn, relation->valuestring, SEM_ROLE_INTENTION);
//             connect_semantic_edge(txn, actor_id, intent_id, "INTENDS", event_id);
//             connect_semantic_edge(txn, event_id, intent_id, "HAS_INTENT", event_id);
//         }
//     }

//     cJSON_Delete(json);
//     return 0;
// }

// int semantic_parse_sentence(const char *sentence, WorkingMemory *wm, MDB_txn *txn) {
//     if (!sentence || !wm || !txn) return -1;

//     char buffer[1024];
//     strncpy(buffer, sentence, sizeof(buffer) - 1);
//     buffer[sizeof(buffer) - 1] = '\0';

//     const char *intent_marker = NULL;
//     const char *cause_marker = NULL;
//     find_connector(buffer, &intent_marker, &cause_marker);

//     char *tokens[32];
//     int token_count = 0;
//     char *tok = strtok(buffer, " \t\r\n.,;!?-\"");
//     while (tok && token_count < 32) {
//         tokens[token_count++] = tok;
//         tok = strtok(NULL, " \t\r\n.,;!?-\"");
//     }

//     if (token_count < 2) return -1;

//     const char *subject = tokens[0];
//     const char *action = token_count > 1 ? tokens[1] : "do";
//     const char *object = token_count > 2 ? tokens[2] : "thing";

//     char event_label[256];
//     snprintf(event_label, sizeof(event_label), "event:%s", sentence);
//     uint64_t event_id = create_semantic_node(txn, event_label, SEM_ROLE_EVENT);

//     uint64_t subject_id = create_semantic_node(txn, subject, SEM_ROLE_AGENT);
//     uint64_t action_id = create_semantic_node(txn, action, SEM_ROLE_ACTION);
//     uint64_t object_id = create_semantic_node(txn, object, SEM_ROLE_PATIENT);

//     connect_semantic_edge(txn, event_id, subject_id, "HAS_ACTOR", event_id);
//     connect_semantic_edge(txn, event_id, action_id, "HAS_ACTION", event_id);
//     connect_semantic_edge(txn, event_id, object_id, "HAS_PATIENT", event_id);

//     if (intent_marker) {
//         char intent_label[128];
//         snprintf(intent_label, sizeof(intent_label), "intention:%s", intent_marker);
//         uint64_t intent_id = create_semantic_node(txn, intent_label, SEM_ROLE_INTENTION);
//         connect_semantic_edge(txn, subject_id, intent_id, "INTENDS", event_id);
//         connect_semantic_edge(txn, event_id, intent_id, "HAS_INTENT", event_id);
//     }

//     if (cause_marker) {
//         char cause_label[128];
//         snprintf(cause_label, sizeof(cause_label), "cause:%s", cause_marker);
//         uint64_t cause_id = create_semantic_node(txn, cause_label, SEM_ROLE_CAUSE);
//         connect_semantic_edge(txn, cause_id, event_id, "CAUSES", event_id);
//     }

//     wm_activate(wm, subject_id, 0.7f, 0.8f);
//     wm_activate(wm, object_id, 0.5f, 0.6f);
//     wm_activate(wm, action_id, 0.8f, 0.7f);

//     return 0;
// }

// // tokenize()
//     // parse_sentence()
//     // parse_json()
void *p;
