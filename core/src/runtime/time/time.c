// runtime/time.c
#include "runtime/time/time.h"

#if defined(_MSC_VER)

#include <intrin.h>

uint64_t vm_rdtsc(void) {
    return __rdtsc();
}

#elif defined(__i386__) || defined(__x86_64__)

uint64_t vm_rdtsc(void) {
    uint32_t lo, hi;

    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));

    return ((uint64_t)hi << 32) | lo;
}

#elif defined(__aarch64__)

static inline uint64_t vm_rdtsc(void) {
    uint64_t v;

    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));

    return v;
}

#else

#include <time.h>

uint64_t vm_rdtsc(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);

    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

#endif
