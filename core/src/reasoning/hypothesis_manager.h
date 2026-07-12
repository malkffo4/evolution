// // гипотезы — это отдельная сущность.
// // Не только генерация.
// // хранение
// // объединение
// // подтверждение
// // отклонение
// // рейтинг
// // reasoning/hypothesis_manager.h

// #ifndef HYPOTHESIS_MANAGER_H
// #define HYPOTHESIS_MANAGER_H

// #include "types.h"

// typedef struct {
//     Triple inferred;
//     float confidence;
//     uint64_t timestamp;
//     uint32_t evidence_count;
//     node_id_t parents[8];
//     ReasoningType reason;
// } Hypothesis;

// ReasoningStrategy { // И хранить это в БД.
//     id;
//     ReasoningType type;

//     char name[64];

//     float min_similarity;
//     float min_coverage;

//     float neighborhood_weight;
//     float center_weight;
//     float relation_weight;
//     float coverage_weight;

//     bool strict;

//     uint32_t successful;
//     uint32_t failed;

//     float average_score;

//     float confidence;
// }

// // abduction
// // ↓
// // hypothesis
// // ↓
// // deduction
// // ↓
// // hypothesis update
// // ↓
// // analogy
// // ↓
// // hypothesis reinforce

// #endif // HYPOTHESIS_MANAGER_H
