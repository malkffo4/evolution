#include <stdint.h>

uint64_t djb2_hash(const char *str) {
    char *ptr = str;
    uint64_t hash = 5381;
    uint32_t c;

    while ((c = *ptr++)) {
        hash = ((hash << 5) + hash) + c;
    }

    return hash;
}
