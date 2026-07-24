// math/hash.c
#include <stdint.h>

#include "storage/hyper_atom/hyper_atom.h"

uint64_t djb2_hash(const char *str) {
    const unsigned char *ptr = (const unsigned char *)str;
    uint64_t hash = 5381;

    while (*ptr) {
        hash = ((hash << 5) + hash) + *ptr++;
    }

    return hash & HYPER_VALUE_MASK;
}
