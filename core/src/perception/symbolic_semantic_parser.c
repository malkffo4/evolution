// #include <stdio.h>
// #include <stdlib.h>
// #include <string.h>
// #include <stdint.h>
// #include <ctype.h>
// #include <math.h>
// #include "../../include/db_core.h"
// #include "../../include/cognition.h"

// #define LEXICON_SIZE 128
// #define SEMANTIC_DIM 64 // 64-битные гипервекторы смысла

// // Грамматические и семантические роли слов
// typedef enum {
//     ROLE_SUBJECT,    // Субъект (Узел А)
//     ROLE_ACTION,     // Действие (Ребро / Связь)
//     ROLE_OBJECT,     // Объект (Узел Б)
//     ROLE_MODIFIER    // Модификатор (Влияет на вес/эмоции)
// } SemanticRole;

// // Базовые семантические измерения (битовые маски смыслов)
// #define SEM_DANGER   (1ULL << 0)  // Разрушение, ошибка, атака
// #define SEM_SYSTEM   (1ULL << 1)  // Операционная система, ядро, софт
// #define SEM_MEMORY   (1ULL << 2)  // Буфер, указатель, выделение, стек
// #define SEM_NETWORK  (1ULL << 3)  // Порт, пакет, сокет, хост
// #define SEM_ACTION   (1ULL << 4)  // Вызов, копирование, отправка, запуск
// #define SEM_CONTROL  (1ULL << 5)  // Проверка, ограничение, валидация
// #define SEM_UTILITY  (1ULL << 6)  // Инструмент, скрипт, полезность

// // Запись во внутреннем семантическом словаре
// typedef struct {
//     char word[32];
//     uint64_t semantic_vector; // Математический вектор смысла (SimHash)
//     float base_danger;        // Базовый когнитивный заряд опасности
//     float base_usefulness;    // Базовый когнитивный заряд полезности
//     SemanticRole role;
// } LexiconEntry;

// // Локальный семантический словарь ИИ (без обращений к сети)
// // Сюда закладываются базовые правила языка, физики, кибербеза, кулинарии и т.д.
// static const LexiconEntry lexicon[LEXICON_SIZE] = {
//     // Кибербез / Системное программирование
//     {"strcpy",          SEM_ACTION | SEM_MEMORY,            0.7f, 0.4f, ROLE_ACTION},
//     {"malloc",          SEM_ACTION | SEM_MEMORY,            0.2f, 0.9f, ROLE_ACTION},
//     {"free",            SEM_ACTION | SEM_MEMORY,            0.1f, 0.9f, ROLE_ACTION},
//     {"buffer_overflow", SEM_DANGER | SEM_MEMORY | SEM_SYSTEM, 0.9f, 0.1f, ROLE_OBJECT},
//     {"memory_leak",     SEM_DANGER | SEM_MEMORY,            0.8f, 0.1f, ROLE_OBJECT},
//     {"stack",           SEM_SYSTEM | SEM_MEMORY,            0.1f, 0.5f, ROLE_SUBJECT},
//     {"heap",            SEM_SYSTEM | SEM_MEMORY,            0.1f, 0.5f, ROLE_SUBJECT},
//     {"port_scan",       SEM_ACTION | SEM_NETWORK,           0.5f, 0.8f, ROLE_ACTION},
//     {"firewall",        SEM_CONTROL | SEM_NETWORK,          0.1f, 0.9f, ROLE_SUBJECT},
//     {"bypass",          SEM_ACTION | SEM_DANGER,            0.7f, 0.8f, ROLE_ACTION},

//     // Кулинария (для проверки универсальности)
//     {"boil",            SEM_ACTION,                         0.1f, 0.6f, ROLE_ACTION},
//     {"water",           SEM_SYSTEM,                         0.0f, 0.8f, ROLE_SUBJECT},
//     {"poison",          SEM_DANGER,                         1.0f, 0.0f, ROLE_OBJECT},
//     {"cook",            SEM_ACTION | SEM_UTILITY,           0.0f, 0.9f, ROLE_ACTION},
//     {"knife",           SEM_UTILITY | SEM_DANGER,           0.4f, 0.8f, ROLE_SUBJECT},

//     // Физика / Схемотехника
//     {"voltage",         SEM_SYSTEM,                         0.3f, 0.7f, ROLE_SUBJECT},
//     {"short_circuit",   SEM_DANGER | SEM_SYSTEM,            0.8f, 0.1f, ROLE_OBJECT},
//     {"fuse",            SEM_CONTROL | SEM_SYSTEM,           0.0f, 0.9f, ROLE_SUBJECT},
//     {"melt",            SEM_ACTION | SEM_DANGER,            0.6f, 0.2f, ROLE_ACTION}
// };

// // Быстрый поиск слова в словаре
// const LexiconEntry* lookup_word(const char *word) {
//     for (int i = 0; i < LEXICON_SIZE; i++) {
//         if (lexicon[i].word[0] == '\0') break;
//         if (strcasecmp(lexicon[i].word, word) == 0) {
//             return &lexicon[i];
//         }
//     }
//     return NULL;
// }

// // Поиск синонимов через расстояние Хэмминга в векторе смысла
// // 0 = идентичный смысл, 64 = противоположный
// int calculate_semantic_distance(uint64_t vec_A, uint64_t vec_B) {
//     uint64_t diff = vec_A ^ vec_B;
//     return __builtin_popcountll(diff);
// }

// // Глубокий математический разбор предложения на Си
// int symbolic_parse_and_ingest(const char *sentence, WorkingMemory *wm, MDB_txn *txn) {
//     char temp[512];
//     strncpy(temp, sentence, sizeof(temp) - 1);
//     temp[sizeof(temp) - 1] = '\0';

//     // Токенизируем строку на слова
//     char *tokens[32];
//     int token_count = 0;
//     char *tok = strtok(temp, " \t\r\n.,;!?-");
//     while (tok && token_count < 32) {
//         tokens[token_count++] = tok;
//         tok = strtok(NULL, " \t\r\n.,;!?-");
//     }

//     if (token_count < 2) return -1; // Слишком короткое предложение для извлечения логики

//     const LexiconEntry *subject = NULL;
//     const LexiconEntry *action = NULL;
//     const LexiconEntry *object = NULL;
//     float modifier_multiplier = 1.0f;

//     // Сканируем токены и распределяем семантические роли
//     for (int i = 0; i < token_count; i++) {
//         const LexiconEntry *entry = lookup_word(tokens[i]);
//         if (!entry) {
//             // Если слова нет в словаре, ИИ динамически создает узел с Novelty = 1.0 (Любопытство)
//             uint64_t unknown_id = djb2_hash(tokens[i]);
//             Node c_node = { .id = unknown_id, .name_hash = add_string_to_pool(txn, tokens[i]), .semantics = 0 };
//             create_node(txn, &c_node);
//             wm_activate(wm, unknown_id, 0.5f, 0.1f);

//             // Находим узел в WM и выставляем Novelty
//             for (uint32_t k = 0; k < wm->count; k++) {
//                 if (wm->nodes[k].node_id == unknown_id) {
//                     wm->nodes[k].emotions.novelty = 1.0f; // Абсолютно новое слово
//                     break;
//                 }
//             }
//             continue;
//         }

//         if (entry->role == ROLE_SUBJECT && !subject) subject = entry;
//         else if (entry->role == ROLE_ACTION && !action) action = entry;
//         else if (entry->role == ROLE_OBJECT && !object) object = entry;
//         else if (entry->role == ROLE_MODIFIER) {
//             // Модификаторы вроде "сильно", "опасно" меняют веса формул
//             modifier_multiplier = 1.5f;
//         }
//     }

//     // Если удалось собрать базовую триаду (Субъект -> Действие -> Объект)
//     if (subject && action && object) {
//         uint64_t sub_id = djb2_hash(subject->word);
//         uint64_t obj_id = djb2_hash(object->word);

//         // --- ВЫЧИСЛЕНИЕ ЭМОЦИЙ И КОГНИТИВНЫХ ЗАРЯДОВ НА СИ ---
//         // Формула опасности: базовый риск субъекта * риск действия * множитель модификатора
//         float calculated_danger = (subject->base_danger + action->base_danger + object->base_danger) / 3.0f * modifier_multiplier;
//         if (calculated_danger > 1.0f) calculated_danger = 1.0f;

//         // Формула полезности (Utility)
//         float calculated_utility = (subject->base_usefulness + action->base_usefulness + object->base_usefulness) / 3.0f;

//         // Формула неопределенности (Uncertainty) - падает при повторениях
//         float calculated_uncertainty = 1.0f - (calculated_danger + calculated_utility) / 2.0f;

//         // Активируем узлы в Working Memory с точным расчетом когнитивных параметров
//         wm_activate(wm, sub_id, 1.0f, calculated_danger);
//         wm_activate(wm, obj_id, 1.0f, calculated_danger);

//         // Прописываем вычисленные эмоции в оперативную память ИИ
//         for (uint32_t i = 0; i < wm->count; i++) {
//             if (wm->nodes[i].node_id == sub_id || wm->nodes[i].node_id == obj_id) {
//                 wm->nodes[i].emotions.danger = calculated_danger;
//                 wm->nodes[i].emotions.usefulness = calculated_utility;
//                 wm->nodes[i].emotions.uncertainty = calculated_uncertainty;
//                 wm->nodes[i].emotions.curiosity = calculated_uncertainty * 0.8f; // Любопытство обратно пропорционально уверенности
//             }
//         }

//         // --- ГИПЕРРАЗМЕРНОЕ СВЯЗЫВАНИЕ (Vector Symbolic Binding) ---
//         // Математическая подпись связи: Signature = Vec(A) XOR Vec(Relation) XOR Vec(B)
//         uint64_t relation_hash = djb2_hash(action->word);
//         uint64_t binding_signature = subject->semantic_vector ^ relation_hash ^ object->semantic_vector;

//         // Сохраняем вычисленный факт в долговременную память (LTM)
//         Edge edge;
//         edge.source = sub_id;
//         edge.target = obj_id;
//         edge.relation = add_string_to_pool(txn, action->word);
//         edge.weight = calculated_danger > 0.5f ? calculated_danger : calculated_utility;
//         edge.context = binding_signature & 0xFFFFFFFF; // Запекаем сигнатуру смысла в контекст связи
//         edge.occurrences = 1;

//         upsert_edge(txn, &edge);

//         printf("[СИМВОЛЬНЫЙ АНАЛИЗ] Понял логику: [%s] --(%s)--> [%s]\n", subject->word, action->word, object->word);
//         printf("  -> Вычисленная Опасность (Danger): %.2f\n", calculated_danger);
//         printf("  -> Вычисленная Полезность (Utility): %.2f\n", calculated_utility);
//         printf("  -> Сигнатура Смысла Связи (VSA Signature): %lu\n", binding_signature);

//         return 0;
//     }

//     return -1; // Не удалось собрать полную смысловую цепь детерминированно
// }
void *p;
