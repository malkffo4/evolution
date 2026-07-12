// runtime/object.c
#include "runtime/object/object.h"

static const VMObjectType object_types[] = {
    {
        .type = OBJECT_EDGESET,
        .name = "EdgeList",
        .destroy = destroy_edges,
        .clone = clone_edges,
        .memory_usage = memory_edges
    },

    {
        .type = OBJECT_NODESET,
        .name = "NodeList",
        .destroy = destroy_nodes,
        .clone = clone_nodes,
        .memory_usage = memory_nodes
    },

    {
        .type = OBJECT_GRAPH,
        .name = "GraphView",
        .destroy = destroy_graph,
        .clone = clone_graph,
        .memory_usage = memory_graph
    },

    {
        .type = OBJECT_STRING,
        .name = "String",
        .destroy = destroy_string,
        .clone = clone_string,
        .memory_usage = memory_string
    }
};

const VMObjectType *vm_object_type_find(ObjectType type) {
    for(size_t i=0;i<ARRAY_SIZE(object_types);i++)
        if(object_types[i].type==type)
            return &object_types[i];

    return NULL;
}

static void destroy_edges(VMObject *obj) {
    EdgeList *list = obj->data;

    if(!list)
        return;

    free(list->items);
    free(list);

    obj->data = NULL;
}

static void destroy_nodes(VMObject *obj) {
    NodeList *list = obj->data;

    if(!list)
        return;

    free(list->items);
    free(list);

    obj->data = NULL;
}

static void destroy_graph(VMObject *obj) {
    GraphView *graph = obj->data;

    if(!graph)
        return;

    free(graph);
    obj->data = NULL;
}

static void destroy_string(VMObject *obj) {
    StringView *str = obj->data;

    if(!str)
        return;

    free((void*)str->data);
    free(str);

    obj->data = NULL;
}

static size_t memory_edges(const VMObject *obj) {
    const EdgeList *list = obj->data;

    if(!list)
        return 0;

    return sizeof(*list) + list->count*sizeof(Edge);
}

static void clone_edges(VMObject *dst, const VMObject *src) {
    EdgeList *a = src->data;

    EdgeList *b = malloc(sizeof(*b));

    *b = *a;

    b->edges = malloc(sizeof(Edge)*a->count);

    memcpy(b->edges, a->edges, sizeof(Edge)*a->count);

    dst->data = b;
}
