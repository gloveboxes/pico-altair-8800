#include "config.h"

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#include <stdio.h>
#include <string.h>

// Flash storage offset (last 4KB sector)
// Pico/Pico W: 2MB flash, Pico 2/Pico 2 W: 4MB flash
// We detect the actual flash size and use the last sector
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (2 * 1024 * 1024) // Default to 2MB if not defined
#endif

#define CONFIG_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define CONFIG_MAGIC 0x43464730 // "CFG0" in hex (new format with RFS IP)

// Cached RFS IP for fast access (loaded on init or after save)
static char cached_rfs_ip[CONFIG_RFS_IP_MAX_LEN + 1] = {0};

// Simple CRC32 implementation
static uint32_t crc32(const uint8_t* data, size_t length)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++)
    {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
        {
            crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
        }
    }
    return ~crc;
}

// Calculate checksum for config (excludes the checksum field itself)
static uint32_t config_calculate_checksum(const config_t* config)
{
    // Calculate CRC over everything except the checksum field
    size_t data_size = offsetof(config_t, checksum);
    return crc32((const uint8_t*)config, data_size);
}

void config_init(void)
{
    // Load cached RFS IP on init
    config_load_rfs_ip(cached_rfs_ip, sizeof(cached_rfs_ip));
}

bool config_exists(void)
{
    // Read the config from flash
    const config_t* flash_config = (const config_t*)(XIP_BASE + CONFIG_FLASH_OFFSET);

    // Check magic number
    if (flash_config->magic != CONFIG_MAGIC)
    {
        return false;
    }

    // Verify checksum
    uint32_t calculated = config_calculate_checksum(flash_config);
    if (calculated != flash_config->checksum)
    {
        return false;
    }

    // Check that SSID is not empty
    if (flash_config->ssid[0] == '\0' || flash_config->ssid[0] == 0xFF)
    {
        return false;
    }

    return true;
}

bool config_load_wifi(char* ssid, size_t ssid_len, char* password, size_t password_len)
{
    if (!ssid || ssid_len == 0 || !password || password_len == 0)
    {
        return false;
    }

    if (!config_exists())
    {
        return false;
    }

    const config_t* flash_config = (const config_t*)(XIP_BASE + CONFIG_FLASH_OFFSET);

    // Copy credentials
    strncpy(ssid, flash_config->ssid, ssid_len - 1);
    ssid[ssid_len - 1] = '\0';

    strncpy(password, flash_config->password, password_len - 1);
    password[password_len - 1] = '\0';

    return true;
}

bool config_load_rfs_ip(char* ip, size_t ip_len)
{
    if (!ip || ip_len == 0)
    {
        return false;
    }

    if (!config_exists())
    {
        ip[0] = '\0';
        return false;
    }

    const config_t* flash_config = (const config_t*)(XIP_BASE + CONFIG_FLASH_OFFSET);

    // Copy RFS IP
    strncpy(ip, flash_config->rfs_server_ip, ip_len - 1);
    ip[ip_len - 1] = '\0';

    return flash_config->rfs_server_ip[0] != '\0';
}

const char* config_get_rfs_ip(void)
{
    return cached_rfs_ip;
}



bool config_save(const char* ssid, const char* password, const char* rfs_ip)
{
    if (!ssid || !password)
    {
        return false;
    }

    // Validate lengths
    size_t ssid_len = strlen(ssid);
    size_t password_len = strlen(password);
    size_t rfs_ip_len = rfs_ip ? strlen(rfs_ip) : 0;

    if (ssid_len == 0 || ssid_len > CONFIG_SSID_MAX_LEN)
    {
        printf("Error: SSID length must be 1-%d characters\n", CONFIG_SSID_MAX_LEN);
        return false;
    }

    if (password_len > CONFIG_PASSWORD_MAX_LEN)
    {
        printf("Error: Password length must be 0-%d characters\n", CONFIG_PASSWORD_MAX_LEN);
        return false;
    }

    if (rfs_ip_len > CONFIG_RFS_IP_MAX_LEN)
    {
        printf("Error: RFS IP length must be 0-%d characters\n", CONFIG_RFS_IP_MAX_LEN);
        return false;
    }

    // Prepare config structure (must be aligned for flash programming)
    static config_t config;
    memset(&config, 0, sizeof(config));
    config.magic = CONFIG_MAGIC;
    strncpy(config.ssid, ssid, CONFIG_SSID_MAX_LEN);
    config.ssid[CONFIG_SSID_MAX_LEN] = '\0';
    strncpy(config.password, password, CONFIG_PASSWORD_MAX_LEN);
    config.password[CONFIG_PASSWORD_MAX_LEN] = '\0';
    if (rfs_ip)
    {
        strncpy(config.rfs_server_ip, rfs_ip, CONFIG_RFS_IP_MAX_LEN);
        config.rfs_server_ip[CONFIG_RFS_IP_MAX_LEN] = '\0';
    }
    config.checksum = config_calculate_checksum(&config);

    // Write to flash - Core 0 should be spinning in RAM during AP mode
    printf("Writing configuration to flash...\n");

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(CONFIG_FLASH_OFFSET, (const uint8_t*)&config, sizeof(config_t));
    restore_interrupts(ints);

    // Update cached RFS IP
    if (rfs_ip)
    {
        strncpy(cached_rfs_ip, rfs_ip, CONFIG_RFS_IP_MAX_LEN);
        cached_rfs_ip[CONFIG_RFS_IP_MAX_LEN] = '\0';
    }
    else
    {
        cached_rfs_ip[0] = '\0';
    }

    printf("Configuration saved successfully\n");
    return true;
}

bool config_clear(void)
{
    printf("Clearing configuration from flash...\n");

    // Erase the sector - interrupts disabled for flash safety
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);

    // Clear cached RFS IP
    cached_rfs_ip[0] = '\0';

    printf("Configuration cleared\n");
    return true;
}
