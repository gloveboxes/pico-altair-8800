/**
 * Remote FS Test Application
 *
 * Tests the remote filesystem client by:
 * 1. Connecting to WiFi
 * 2. Connecting to remote_fs_server.py
 * 3. Performing INIT, READ, WRITE operations
 * 4. Verifying responses
 *
 * Build: cd test/remote_fs && mkdir build && cd build && cmake .. && make
 * Run: Flash remote_fs_test.uf2, monitor via USB serial
 */

#include <stdio.h>
#include <string.h>

#include "pico/cyw43_arch.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "remote_fs.h"

// WiFi credentials - set these for your network
#ifndef WIFI_SSID
#define WIFI_SSID "YOUR_WIFI_SSID"
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#endif

// Test configuration
#define TEST_DRIVE 0
#define TEST_TRACK 0
#define TEST_SECTOR 0

// Test state
static volatile bool wifi_connected = false;
static volatile bool test_complete = false;
static int tests_passed = 0;
static int tests_failed = 0;

// Forward declarations
static void core1_entry(void);
static bool run_tests(void);

// ============================================================================
// Test Helpers
// ============================================================================

static void test_pass(const char* name)
{
    printf("  [PASS] %s\n", name);
    tests_passed++;
}

static void test_fail(const char* name, const char* reason)
{
    printf("  [FAIL] %s - %s\n", name, reason);
    tests_failed++;
}

// ============================================================================
// Core 1 - Network Operations
// ============================================================================

static void core1_entry(void)
{
    printf("[Core1] Starting network operations\n");

    // Initialize WiFi
    if (cyw43_arch_init())
    {
        printf("[Core1] CYW43 init failed\n");
        return;
    }

    cyw43_arch_enable_sta_mode();

    printf("[Core1] Connecting to WiFi '%s'...\n", WIFI_SSID);

    int err = cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000);

    if (err != 0)
    {
        printf("[Core1] WiFi connect failed: %d\n", err);
        return;
    }

    printf("[Core1] WiFi connected\n");
    wifi_connected = true;

    // Main poll loop
    while (!test_complete)
    {
        cyw43_arch_poll();
        rfs_client_poll();
        sleep_ms(1);
    }

    printf("[Core1] Exiting\n");
}

// ============================================================================
// Tests
// ============================================================================

static bool test_connect(void)
{
    printf("Testing: Connect to server\n");

    if (!rfs_request_connect())
    {
        test_fail("Connect", "Failed to queue request");
        return false;
    }

    // Wait for connection
    uint32_t timeout = 15000;
    uint32_t start = to_ms_since_boot(get_absolute_time());

    while (!rfs_client_is_ready() && !rfs_client_has_error())
    {
        if (to_ms_since_boot(get_absolute_time()) - start > timeout)
        {
            test_fail("Connect", "Timeout");
            return false;
        }
        sleep_ms(10);
    }

    if (rfs_client_has_error())
    {
        test_fail("Connect", "Connection error");
        return false;
    }

    // Get INIT response
    rfs_response_t response;
    if (rfs_get_response(&response))
    {
        if (response.op == RFS_OP_INIT && response.status == RFS_RESP_OK)
        {
            test_pass("Connect");
            return true;
        }
    }

    test_fail("Connect", "Bad INIT response");
    return false;
}

static bool test_read_sector(void)
{
    printf("Testing: Read sector (drive=%d, track=%d, sector=%d)\n", TEST_DRIVE, TEST_TRACK, TEST_SECTOR);

    if (!rfs_request_read(TEST_DRIVE, TEST_TRACK, TEST_SECTOR))
    {
        test_fail("Read", "Failed to queue request");
        return false;
    }

    // Wait for response
    uint32_t timeout = 5000;
    uint32_t start = to_ms_since_boot(get_absolute_time());
    rfs_response_t response;

    while (!rfs_get_response(&response))
    {
        if (to_ms_since_boot(get_absolute_time()) - start > timeout)
        {
            test_fail("Read", "Timeout");
            return false;
        }
        sleep_ms(10);
    }

    if (response.status != RFS_RESP_OK)
    {
        test_fail("Read", "Server returned error");
        return false;
    }

    // Print first few bytes
    printf("  Data: ");
    for (int i = 0; i < 16 && i < RFS_SECTOR_SIZE; i++)
    {
        printf("%02X ", response.data[i]);
    }
    printf("...\n");

    test_pass("Read");
    return true;
}

static bool test_write_sector(void)
{
    printf("Testing: Write sector (drive=%d, track=%d, sector=%d)\n", TEST_DRIVE, TEST_TRACK, TEST_SECTOR);

    // Create test data
    uint8_t test_data[RFS_SECTOR_SIZE];
    for (int i = 0; i < RFS_SECTOR_SIZE; i++)
    {
        test_data[i] = (uint8_t)(i & 0xFF);
    }

    if (!rfs_request_write(TEST_DRIVE, TEST_TRACK, TEST_SECTOR, test_data))
    {
        test_fail("Write", "Failed to queue request");
        return false;
    }

    // Wait for response
    uint32_t timeout = 5000;
    uint32_t start = to_ms_since_boot(get_absolute_time());
    rfs_response_t response;

    while (!rfs_get_response(&response))
    {
        if (to_ms_since_boot(get_absolute_time()) - start > timeout)
        {
            test_fail("Write", "Timeout");
            return false;
        }
        sleep_ms(10);
    }

    if (response.status != RFS_RESP_OK)
    {
        test_fail("Write", "Server returned error");
        return false;
    }

    test_pass("Write");
    return true;
}

static bool test_read_verify(void)
{
    printf("Testing: Read and verify written data\n");

    if (!rfs_request_read(TEST_DRIVE, TEST_TRACK, TEST_SECTOR))
    {
        test_fail("ReadVerify", "Failed to queue request");
        return false;
    }

    // Wait for response
    uint32_t timeout = 5000;
    uint32_t start = to_ms_since_boot(get_absolute_time());
    rfs_response_t response;

    while (!rfs_get_response(&response))
    {
        if (to_ms_since_boot(get_absolute_time()) - start > timeout)
        {
            test_fail("ReadVerify", "Timeout");
            return false;
        }
        sleep_ms(10);
    }

    if (response.status != RFS_RESP_OK)
    {
        test_fail("ReadVerify", "Server returned error");
        return false;
    }

    // Verify data matches what we wrote
    for (int i = 0; i < RFS_SECTOR_SIZE; i++)
    {
        if (response.data[i] != (uint8_t)(i & 0xFF))
        {
            printf("  Mismatch at byte %d: expected %02X, got %02X\n", i, (uint8_t)(i & 0xFF), response.data[i]);
            test_fail("ReadVerify", "Data mismatch");
            return false;
        }
    }

    test_pass("ReadVerify");
    return true;
}

static bool run_tests(void)
{
    printf("\n========================================\n");
    printf("Remote FS Test Suite\n");
    printf("Server: %s:%d\n", RFS_SERVER_IP, RFS_SERVER_PORT);
    printf("========================================\n\n");

    // Initialize remote FS client (must be before Core 1 starts polling)
    rfs_client_init();

    // Wait for WiFi
    printf("Waiting for WiFi...\n");
    while (!wifi_connected)
    {
        sleep_ms(100);
    }
    printf("WiFi ready\n\n");

    // Run tests
    bool all_passed = true;

    all_passed &= test_connect();
    if (!rfs_client_is_ready())
    {
        printf("\nConnection failed, cannot continue tests\n");
        return false;
    }

    all_passed &= test_read_sector();
    all_passed &= test_write_sector();
    all_passed &= test_read_verify();

    // Summary
    printf("\n========================================\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("========================================\n");

    return all_passed;
}

// ============================================================================
// Main
// ============================================================================

int main(void)
{
    stdio_init_all();

    // Wait for USB serial to connect
    sleep_ms(2000);

    printf("\n\n");
    printf("=====================================\n");
    printf("Remote FS Test Application\n");
    printf("=====================================\n\n");

    // Launch Core 1 for network operations
    multicore_launch_core1(core1_entry);

    // Run tests on Core 0
    bool success = run_tests();

    // Signal Core 1 to exit
    test_complete = true;
    sleep_ms(100);

    if (success)
    {
        printf("\n*** ALL TESTS PASSED ***\n");
    }
    else
    {
        printf("\n*** SOME TESTS FAILED ***\n");
    }

    // Blink LED to indicate completion
    while (true)
    {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
        sleep_ms(success ? 500 : 100);
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
        sleep_ms(success ? 500 : 100);
    }

    return 0;
}
