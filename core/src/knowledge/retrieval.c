// RAG (Retrieval-Augmented Generation)
#include <stdio.h>
#include <stdlib.h>

#include "types.h"
#include "storage/edge.h"

// Функция для извлечения субграфа по узлу
int retrieve_subgraph(node_id_t node_id, Node **nodes, Edge **edges, int *count) {
    // Явное указание компилятору, что переменные пока не используются
    (void)node_id;
    (void)nodes;
    (void)edges;
    (void)count;

    // TODO: Реализовать алгоритм извлечения подграфа
    return 0;
}
