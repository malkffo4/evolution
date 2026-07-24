// runtime/object/object.c
#include <string.h>
#include <stdlib.h>
#include <stddef.h>

#include "runtime/object/object.h"
#include "storage/graph/graph.h"
#include "runtime/compiler/pipeline.h"

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

// Forward declarations
static void destroy_edges(VMObject *obj);
static void destroy_nodes(VMObject *obj);
static void destroy_graph(VMObject *obj);
static void destroy_string(VMObject *obj);
static void clone_edges(VMObject *dst, const VMObject *src);
static void clone_nodes(VMObject *dst, const VMObject *src);
static void clone_graph(VMObject *dst, const VMObject *src);
static void clone_string(VMObject *dst, const VMObject *src);
static size_t memory_edges(const VMObject *obj);
static size_t memory_nodes(const VMObject *obj);
static size_t memory_graph(const VMObject *obj);
static size_t memory_string(const VMObject *obj);

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
    for(size_t i=0; i<ARRAY_SIZE(object_types); i++)
        if(object_types[i].type == type)
            return &object_types[i];
    return NULL;
}

static void destroy_edges(VMObject *obj) {
    EdgeList *list = obj->data;
    if(!list) return;
    free(list->items);
    free(list);
    obj->data = NULL;
}

static void destroy_nodes(VMObject *obj) {
    NodeList *list = obj->data;
    if(!list) return;
    free(list->items);
    free(list);
    obj->data = NULL;
}

static void destroy_graph(VMObject *obj) {
    GraphView *graph = obj->data;
    if(!graph) return;
    free(graph);
    obj->data = NULL;
}

static void destroy_string(VMObject *obj) {
    StringView *str = obj->data;
    if(!str) return;
    free((void*)str->data);
    free(str);
    obj->data = NULL;
}

static size_t memory_edges(const VMObject *obj) {
    const EdgeList *list = obj->data;
    if(!list) return 0;
    return sizeof(*list) + list->count * sizeof(Edge);
}

static size_t memory_nodes(const VMObject *obj) {
    const NodeList *list = obj->data;
    if(!list) return 0;
    return sizeof(*list) + list->count * sizeof(node_id_t);
}

static size_t memory_graph(const VMObject *obj) {
    (void)obj;
    return sizeof(GraphView);
}

static size_t memory_string(const VMObject *obj) {
    const StringView *str = obj->data;
    if(!str) return 0;
    return sizeof(StringView) + str->len + 1;
}

static void clone_edges(VMObject *dst, const VMObject *src) {
    EdgeList *a = src->data;
    EdgeList *b = malloc(sizeof(*b));
    *b = *a;
    b->items = malloc(sizeof(Edge) * a->count);
    memcpy(b->items, a->items, sizeof(Edge) * a->count);
    dst->data = b;
}

static void clone_nodes(VMObject *dst, const VMObject *src) {
    NodeList *a = src->data;
    NodeList *b = malloc(sizeof(*b));
    *b = *a;
    b->items = malloc(sizeof(node_id_t) * a->count);
    memcpy(b->items, a->items, sizeof(node_id_t) * a->count);
    dst->data = b;
}

static void clone_graph(VMObject *dst, const VMObject *src) {
    GraphView *a = src->data;
    GraphView *b = malloc(sizeof(*b));
    *b = *a;
    // Клонируем вложенные списки
    if (b->nodes.items) {
        b->nodes.items = malloc(sizeof(node_id_t) * b->nodes.count);
        memcpy(b->nodes.items, a->nodes.items, sizeof(node_id_t) * b->nodes.count);
    }
    if (b->edges.items) {
        b->edges.items = malloc(sizeof(Edge) * b->edges.count);
        memcpy(b->edges.items, a->edges.items, sizeof(Edge) * b->edges.count);
    }
    dst->data = b;
}

static void clone_string(VMObject *dst, const VMObject *src) {
    StringView *a = src->data;
    StringView *b = malloc(sizeof(*b));
    *b = *a;
    b->data = strdup(a->data);
    dst->data = b;
}
