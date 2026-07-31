// math/hash.h
#pragma once

#include <stdint.h>
#include <stddef.h>

uint64_t djb2_hash(const char *str);
uint64_t fnv1a_hash(const void *data, size_t size);
uint64_t murmur_hash64(const void *data, size_t size);
