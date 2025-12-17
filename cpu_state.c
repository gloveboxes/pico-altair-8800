#include "cpu_state.h"
#include <stdio.h>

volatile CPU_OPERATING_MODE g_cpu_mode = CPU_STOPPED;

void cpu_state_set_mode(CPU_OPERATING_MODE mode)
{
    g_cpu_mode = mode;
    
    // Optional: Add logging for debugging
    const char *mode_str;
    switch (mode)
    {
        case CPU_RUNNING:
            mode_str = "RUNNING";
            break;
        case CPU_STOPPED:
            mode_str = "STOPPED";
            break;
        case CPU_LOW_POWER:
            mode_str = "LOW_POWER";
            break;
        default:
            mode_str = "UNKNOWN";
            break;
    }
    printf("CPU mode set to %s\n", mode_str);
}
