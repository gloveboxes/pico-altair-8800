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

    // Prepare config structure
    config_t config = {0};
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

    // Erase and write flash sector
    // CRITICAL: Interrupts must be disabled during flash operations
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

    // Erase the sector
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);

    // Clear cached RFS IP
    cached_rfs_ip[0] = '\0';

    printf("Configuration cleared\n");
    return true;
}

// Helper function to read a line of input with echo
static bool read_input_line(char* buffer, size_t max_len, bool echo_asterisks)
{
    size_t idx = 0;

    while (idx < max_len)
    {
        int c = getchar_timeout_us(60 * 1000 * 1000); // 60 second timeout
        if (c == PICO_ERROR_TIMEOUT)
        {
            printf("\nTimeout - configuration cancelled\n\n");
            return false;
        }

        if (c == '\r' || c == '\n')
        {
            printf("\n");
            break;
        }
        else if (c == 0x7F || c == 0x08) // Backspace/Delete
        {
            if (idx > 0)
            {
                idx--;
                buffer[idx] = '\0';
                printf("\b \b"); // Erase character from display
            }
        }
        else if (c >= 0x20 && c < 0x7F) // Printable ASCII
        {
            buffer[idx++] = (char)c;
            putchar(echo_asterisks ? '*' : c);
        }
    }

    buffer[idx] = '\0';
    return true;
}

bool config_prompt_and_save(uint32_t timeout_ms)
{
    printf("\n");
    printf("========================================\n");
    printf("  System Configuration\n");
    printf("========================================\n");
    printf("\n");
    printf("Press 'Y' within %lu seconds to enter configuration...\n", (unsigned long)(timeout_ms / 1000));
    printf("Press ENTER to skip and continue...\n");

    // Wait for 'Y' input with timeout
    absolute_time_t start_time = get_absolute_time();
    absolute_time_t last_dot_time = start_time;
    bool configure = false;

    while (absolute_time_diff_us(start_time, get_absolute_time()) < (int64_t)(timeout_ms * 1000))
    {
        // Print a dot every second
        absolute_time_t current_time = get_absolute_time();
        if (absolute_time_diff_us(last_dot_time, current_time) >= 1000000)
        {
            printf(".");
            fflush(stdout);
            last_dot_time = current_time;
        }

        int c = getchar_timeout_us(10000); // Check every 10ms
        if (c != PICO_ERROR_TIMEOUT)
        {
            if (c == 'Y' || c == 'y')
            {
                configure = true;
                printf("\nY\n");
                break;
            }
            else if (c == '\r' || c == '\n')
            {
                printf("\nSkipping configuration\n\n");
                return false;
            }
        }
        tight_loop_contents();
    }

    if (!configure)
    {
        printf("\nTimeout - skipping configuration\n\n");
        return false;
    }

    // ========== WiFi SSID ==========
    printf("\n--- WiFi Configuration ---\n");
    printf("Enter WiFi SSID (max %d characters): ", CONFIG_SSID_MAX_LEN);
    char ssid[CONFIG_SSID_MAX_LEN + 1] = {0};

    if (!read_input_line(ssid, CONFIG_SSID_MAX_LEN, false))
    {
        return false;
    }

    if (ssid[0] == '\0')
    {
        printf("Error: SSID cannot be empty\n\n");
        return false;
    }

    // ========== WiFi Password (with confirmation) ==========
    char password[CONFIG_PASSWORD_MAX_LEN + 1] = {0};
    char password_confirm[CONFIG_PASSWORD_MAX_LEN + 1] = {0};
    bool passwords_match = false;

    while (!passwords_match)
    {
        printf("Enter WiFi password (max %d characters): ", CONFIG_PASSWORD_MAX_LEN);
        memset(password, 0, sizeof(password));

        if (!read_input_line(password, CONFIG_PASSWORD_MAX_LEN, true))
        {
            return false;
        }

        printf("Confirm WiFi password: ");
        memset(password_confirm, 0, sizeof(password_confirm));

        if (!read_input_line(password_confirm, CONFIG_PASSWORD_MAX_LEN, true))
        {
            return false;
        }

        if (strcmp(password, password_confirm) == 0)
        {
            passwords_match = true;
        }
        else
        {
            printf("Error: Passwords do not match. Please try again.\n\n");
        }
    }

    // ========== Remote FS Server IP ==========
    printf("\n--- Remote FS Configuration ---\n");
    printf("Enter Remote FS server IP (e.g., 192.168.1.100, or leave empty to skip): ");
    char rfs_ip[CONFIG_RFS_IP_MAX_LEN + 1] = {0};

    if (!read_input_line(rfs_ip, CONFIG_RFS_IP_MAX_LEN, false))
    {
        return false;
    }

    // ========== Save Configuration ==========
    printf("\n");
    printf("Saving configuration:\n");
    printf("  WiFi SSID: %s\n", ssid);
    printf("  RFS Server IP: %s\n", rfs_ip[0] ? rfs_ip : "(not configured)");

    if (config_save(ssid, password, rfs_ip[0] ? rfs_ip : NULL))
    {
        printf("Configuration saved successfully!\n\n");
        return true;
    }
    else
    {
        printf("Failed to save configuration\n\n");
        return false;
    }
}
