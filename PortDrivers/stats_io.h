/**
 * @file stats_io.h
 * @brief Statistics I/O port driver for Altair 8800 emulator
 *
 * Port 50: lwIP memory pool statistics
 * Port 51: Remote FS cache statistics
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * @brief lwIP Stats data type enumeration for port 40
 *
 * Each value selects a different lwIP statistics category.
 */
typedef enum
{
    STATS_HEAP = 0, /**< Heap max/error: "[LWIP] Heap max:%u err:%u" */
    STATS_PBUF = 1, /**< PBUF pool stats: "[LWIP] PBUF:%u/%u(max %u,err %u)" */
    STATS_SEG = 2,  /**< TCP segment stats: "[LWIP] SEG:%u/%u(max %u,err %u)" */
    STATS_PCB = 3,  /**< TCP PCB stats: "[LWIP] PCB:%u/%u(max %u,err %u)" */
    STATS_COUNT     /**< Number of stats types */
} stats_type_t;

/**
 * @brief RFS Stats data type enumeration for port 41
 *
 * Each value selects a different Remote FS cache statistic.
 */
typedef enum
{
    RFS_STATS_CACHE = 0, /**< Cache stats: "[RFS] Hits:%u Miss:%u Rate:%u%% Skips:%u" */
    RFS_STATS_COUNT      /**< Number of RFS stats types */
} rfs_stats_type_t;

/**
 * @brief Handle output to stats port 50 (lwIP) or 51 (RFS)
 *
 * @param port Port number (50=lwIP, 51=RFS)
 * @param data Stats type selector (see stats_type_t or rfs_stats_type_t enum)
 * @param buffer Output buffer for formatted string
 * @param buffer_length Size of output buffer
 * @return Number of bytes written to buffer
 */
size_t stats_output(int port, uint8_t data, char* buffer, size_t buffer_length);

/**
 * @brief Handle input from stats port (not used)
 *
 * @param port Port number
 * @return Always returns 0
 */
uint8_t stats_input(uint8_t port);
