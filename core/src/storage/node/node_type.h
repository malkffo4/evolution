// storage/node/node_type.h
#ifndef NODE_TYPE_H
#define NODE_TYPE_H

typedef enum {
    NODE_CONCEPT,
    NODE_EPISODE,
    NODE_RULE,
    NODE_ALGORITHM,
    NODE_PLAN,
    NODE_HYPOTHESIS,
    NODE_OBSERVATION,
    NODE_GOAL,
    NODE_SKILL,
    NODE_ERROR,
    NODE_LESSON
} NodeType;

#endif // NODE_TYPE_H
