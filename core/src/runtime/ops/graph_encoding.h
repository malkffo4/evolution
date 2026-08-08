// runtime/ops/graph_encoding.h
#pragma once

#include <stdint.h>

#define GRAPH_INSTR_FIELD_BITS 10
#define GRAPH_INSTR_FIELD_MASK 0x3FFULL   // 10 бит на поле, 6 полей = 60 бит

static inline uint64_t graph_pack_args(const uint32_t arg[6]) {
    uint64_t packed = 0;
    for (int i = 0; i < 6; i++)
        packed |= ((uint64_t)(arg[i] & GRAPH_INSTR_FIELD_MASK)) << (i * GRAPH_INSTR_FIELD_BITS);
    return packed;
}

static inline void graph_unpack_args(uint64_t packed, uint32_t out[6]) {
    for (int i = 0; i < 6; i++)
        out[i] = (uint32_t)((packed >> (i * GRAPH_INSTR_FIELD_BITS)) & GRAPH_INSTR_FIELD_MASK);
}
