#pragma once

#include <stdint.h>
#include <time.h>

typedef uint64_t absolute_time_t;

static inline absolute_time_t get_absolute_time(void)
{
    struct timespec ts;
    static uint64_t start_us = 0;
    uint64_t now_us;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    now_us = ((uint64_t)ts.tv_sec * 1000000ULL) + ((uint64_t)ts.tv_nsec / 1000ULL);
    if (start_us == 0)
    {
        start_us = now_us;
    }
    return now_us - start_us;
}

static inline uint64_t to_ms_since_boot(absolute_time_t t)
{
    return t / 1000ULL;
}
