/**
 * @file stats_io.c
 * @brief Statistics I/O port driver for Altair 8800 emulator
 *
 * Port 50: lwIP statistics
 * Port 51: Remote FS cache statistics
 */

#include "PortDrivers/stats_io.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// Include cyw43 header first to get CYW43_WL_GPIO_LED_PIN define
#if __has_include("pico/cyw43_arch.h")
#include "pico/cyw43_arch.h"
#endif

#ifdef CYW43_WL_GPIO_LED_PIN
#include "lwip/stats.h"
#include "lwipopts.h"
#endif

#include "Altair8800/remote_fs.h"
#include "stats_io.h"

// Forward declarations for lwIP stats helper
static size_t lwip_stats_output(uint8_t data, char* buffer, size_t buffer_length);
static size_t rfs_stats_output(uint8_t data, char* buffer, size_t buffer_length);

size_t stats_output(int port, uint8_t data, char* buffer, size_t buffer_length)
{
    if (buffer == NULL || buffer_length == 0)
    {
        return 0;
    }

    switch (port)
    {
        case 50:
            return lwip_stats_output(data, buffer, buffer_length);
        case 51:
            return rfs_stats_output(data, buffer, buffer_length);
        default:
            return (size_t)snprintf(buffer, buffer_length, "[STATS] Unknown port: %d", port);
    }
}

static size_t lwip_stats_output(uint8_t data, char* buffer, size_t buffer_length)
{
    size_t len = 0;

#ifdef CYW43_WL_GPIO_LED_PIN
    switch (data)
    {
        case STATS_HEAP:
            len = (size_t)snprintf(buffer, buffer_length, "[LWIP] Heap max:%u err:%u", (unsigned)lwip_stats.mem.max,
                                   (unsigned)lwip_stats.mem.err);
            break;

        case STATS_PBUF:
            len = (size_t)snprintf(
                buffer, buffer_length, "[LWIP] PBUF:%u/%u(max %u,err %u)",
                (unsigned)lwip_stats.memp[MEMP_PBUF_POOL]->used, (unsigned)lwip_stats.memp[MEMP_PBUF_POOL]->avail,
                (unsigned)lwip_stats.memp[MEMP_PBUF_POOL]->max, (unsigned)lwip_stats.memp[MEMP_PBUF_POOL]->err);
            break;

        case STATS_SEG:
            len = (size_t)snprintf(
                buffer, buffer_length, "[LWIP] SEG:%u/%u(max %u,err %u)", (unsigned)lwip_stats.memp[MEMP_TCP_SEG]->used,
                (unsigned)lwip_stats.memp[MEMP_TCP_SEG]->avail, (unsigned)lwip_stats.memp[MEMP_TCP_SEG]->max,
                (unsigned)lwip_stats.memp[MEMP_TCP_SEG]->err);
            break;

        case STATS_PCB:
            len = (size_t)snprintf(
                buffer, buffer_length, "[LWIP] PCB:%u/%u(max %u,err %u)", (unsigned)lwip_stats.memp[MEMP_TCP_PCB]->used,
                (unsigned)lwip_stats.memp[MEMP_TCP_PCB]->avail, (unsigned)lwip_stats.memp[MEMP_TCP_PCB]->max,
                (unsigned)lwip_stats.memp[MEMP_TCP_PCB]->err);
            break;

        default:
            len = (size_t)snprintf(buffer, buffer_length, "[LWIP] Unknown stat type: %u", data);
            break;
    }
#else
    (void)data;
    len = (size_t)snprintf(buffer, buffer_length, "[LWIP] Stats not available");
#endif

    return len;
}

static size_t rfs_stats_output(uint8_t data, char* buffer, size_t buffer_length)
{
    size_t len = 0;

#ifdef REMOTE_FS_SUPPORT
    switch (data)
    {
        case RFS_STATS_CACHE:
        {
            uint32_t hits, misses, write_skips;
            rfs_get_cache_stats(&hits, &misses, &write_skips);
            uint32_t total = hits + misses;
            uint32_t rate = (total > 0) ? ((hits * 100) / total) : 0;
            len = (size_t)snprintf(buffer, buffer_length, "[RFS] Hits:%u Miss:%u Rate:%u%% Skips:%u", (unsigned)hits,
                                   (unsigned)misses, (unsigned)rate, (unsigned)write_skips);
            break;
        }

        default:
            len = (size_t)snprintf(buffer, buffer_length, "[RFS] Unknown stat type: %u", data);
            break;
    }
#else
    (void)data;
    len =
        (size_t)snprintf(buffer, buffer_length, "[RFS] Not available (SD Card mode or Embbedded Disk mode is enabled)");
#endif

    return len;
}

uint8_t stats_input(uint8_t port)
{
    (void)port;
    return 0;
}
