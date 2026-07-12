// поиск шаблонов в графе
// reasoning/pattern.h
#ifndef PATTERN_H
#define PATTERN_H

typedef struct
{
    EdgeList edges;

} Pattern;

typedef struct
{
    node_id_t root;

    EdgeMatch *matches;

    int count;

    float score;

} PatternMatch;

int pattern_match(
    MDB_txn *txn,
    const Pattern *pattern,
    PatternMatch **matches,
    int *count);

#endif // PATTERN_H
// find_chain()

// find_triangle()

// find_cycle()

// find_subgraph()

// find_relation()

// find_pattern()

// Тогда deduction работает поверх него.
// analogy тоже.
// contradiction тоже.
// Это сильно уменьшит дублирование.
