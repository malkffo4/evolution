#ifndef COGNITIVE_H
#define COGNITIVE_H

#define ACTIVATION_THRESHOLD 0.4f
#define DECAY_RATE 0.15f  // Как быстро ИИ забывает прошлый шаг (для экономии памяти i3)


typedef struct {
    float curiosity;
    float confidence;
    float novelty;
    float usefulness;
    float uncertainty;
    float expected_reward;
    float risk;
    // float surprise;
    // float excitement;
    // float anxiety;
    // float frustration;
    // float satisfaction;
    // float boredom;
    // float engagement;
    // float focus;
    // float fatigue;
    // float stress;
    // float happiness;
    // float sadness;
    // float anger;
    // float fear;
    // float disgust;
    // float anticipation;
    // float trust;
    // float love;
    // float shame;
    // float guilt;
    // float pride;
    float danger;
    float urgency;
} CognitiveValue;

typedef struct {
    float novelty;   // Любопытство (насколько узел новый/неизученный)
    float danger;    // Страх/Опасность (риск падения системы или бана со стороны WAF)
    float utility;   // Радость/Польза (насколько этот факт приближает к цели/деньгам)
} CognitiveEmotions;

typedef struct {
    node_id_t id;
    NodeType type;
    CognitiveValue state;
    uint64_t created_at;
    uint64_t updated_at;
    uint16_t flags;
} CognitiveObject;

#endif // COGNITIVE_H
