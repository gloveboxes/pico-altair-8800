#ifndef CPU_STATE_H
#define CPU_STATE_H

#include <stdint.h>

typedef enum
{
    CPU_RUNNING = 1,
    CPU_STOPPED = 2,
    CPU_LOW_POWER = 3
} CPU_OPERATING_MODE;

// Set the CPU operating mode
void cpu_state_set_mode(CPU_OPERATING_MODE mode);

// Inline wrapper for high-performance reads
static inline CPU_OPERATING_MODE cpu_state_get_mode(void)
{
    extern volatile CPU_OPERATING_MODE g_cpu_mode;
    return g_cpu_mode;
}

#endif // CPU_STATE_H
