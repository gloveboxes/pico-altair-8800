#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Maximum lengths for configuration fields
#define CONFIG_SSID_MAX_LEN 32
#define CONFIG_PASSWORD_MAX_LEN 63
#define CONFIG_RFS_IP_MAX_LEN 15 // "xxx.xxx.xxx.xxx"

// Configuration structure stored in flash
typedef struct
{
    uint32_t magic;                                // Magic number for validation (0x43464730 = "CFG0")
    char ssid[CONFIG_SSID_MAX_LEN + 1];            // WiFi SSID (null-terminated)
    char password[CONFIG_PASSWORD_MAX_LEN + 1];    // WiFi Password (null-terminated)
    char rfs_server_ip[CONFIG_RFS_IP_MAX_LEN + 1]; // Remote FS server IP (null-terminated)
    uint32_t checksum;                             // CRC32 checksum for validation
} config_t;

// Initialize the configuration system
void config_init(void);

// Check if valid configuration is stored in flash
bool config_exists(void);

// Load WiFi credentials from flash
// Returns true if valid credentials were loaded
bool config_load_wifi(char* ssid, size_t ssid_len, char* password, size_t password_len);

// Load Remote FS server IP from flash
// Returns true if valid IP was loaded
bool config_load_rfs_ip(char* ip, size_t ip_len);

// Get Remote FS server IP (returns empty string if not configured)
const char* config_get_rfs_ip(void);

// Save all configuration to flash
// Returns true if successfully saved
bool config_save(const char* ssid, const char* password, const char* rfs_ip);

// Clear configuration from flash
// Returns true if successfully cleared
bool config_clear(void);
