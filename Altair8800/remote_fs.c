#include "remote_fs.h"

#include "pico/stdlib.h"

// Remote FS is only available on WiFi-enabled boards
#if defined(CYW43_WL_GPIO_LED_PIN)

#include <stdio.h>
#include <string.h>

#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "pico/cyw43_arch.h"
#include "pico/util/queue.h"

// Server configuration (set via CMake)
#ifndef RFS_SERVER_IP
#define RFS_SERVER_IP "192.168.1.22"
#endif

#ifndef RFS_SERVER_PORT
#define RFS_SERVER_PORT 8080
#endif

// Queue sizes
#define RFS_OUTBOUND_QUEUE_SIZE 4
#define RFS_INBOUND_QUEUE_SIZE 4

// Receive buffer size (status + sector data)
#define RFS_RECV_BUF_SIZE (1 + RFS_SECTOR_SIZE)

// Connection timeout (ms)
#define RFS_CONNECT_TIMEOUT_MS 15000
#define RFS_OPERATION_TIMEOUT_MS 20000
#define RFS_MAX_RETRIES 3
#define RFS_RECONNECT_DELAY_MS 500 // Reduced from 1000ms for faster recovery

// Queues for inter-core communication
static queue_t rfs_outbound_queue; // Core 0 -> Core 1
static queue_t rfs_inbound_queue;  // Core 1 -> Core 0

// Client state
static struct
{
    rfs_client_state_t state;
    struct tcp_pcb* pcb;

    // Current operation
    rfs_request_t current_request;
    bool request_in_progress;

    // Receive buffer
    uint8_t recv_buf[RFS_RECV_BUF_SIZE];
    size_t recv_len;
    size_t expected_len;

    // Timing
    uint32_t operation_start_time;

    // Auto-recovery
    bool has_pending_retry;
    rfs_request_t pending_retry_request;
    uint8_t retry_count;
    uint32_t reconnect_start_time;
} client;

// Static buffer for write requests to avoid stack allocation
static uint8_t rfs_write_buf[4 + RFS_SECTOR_SIZE];

// ============================================================================
// Sector Cache - 100KB write-through cache shared across all drives
// ============================================================================

// Cache configuration
#define RFS_CACHE_SIZE_BYTES (160 * 1024) // 160KB

// Cache entry structure (148 bytes each)
typedef struct
{
    uint8_t drive;
    uint8_t track;
    uint8_t sector;
    uint8_t valid; // 1 if entry contains valid data
    uint32_t age;  // LRU counter - higher = more recently used
    uint8_t data[RFS_SECTOR_SIZE];
} rfs_cache_entry_t;

#define RFS_CACHE_ENTRY_SIZE (sizeof(rfs_cache_entry_t))
#define RFS_CACHE_NUM_ENTRIES (RFS_CACHE_SIZE_BYTES / RFS_CACHE_ENTRY_SIZE)

// Static cache pool - shared across all drives
static rfs_cache_entry_t rfs_cache[RFS_CACHE_NUM_ENTRIES];
static uint32_t rfs_cache_age_counter = 0;
static uint32_t rfs_cache_hits = 0;
static uint32_t rfs_cache_misses = 0;
static uint32_t rfs_cache_write_skips = 0; // Writes skipped due to identical data

// Find entry in cache (returns NULL if not found)
static rfs_cache_entry_t* rfs_cache_find(uint8_t drive, uint8_t track, uint8_t sector)
{
    for (size_t i = 0; i < RFS_CACHE_NUM_ENTRIES; i++)
    {
        if (rfs_cache[i].valid && rfs_cache[i].drive == drive && rfs_cache[i].track == track &&
            rfs_cache[i].sector == sector)
        {
            // Update LRU age
            rfs_cache[i].age = ++rfs_cache_age_counter;
            return &rfs_cache[i];
        }
    }
    return NULL;
}

// Find LRU entry to evict (or first empty slot)
static rfs_cache_entry_t* rfs_cache_find_lru(void)
{
    rfs_cache_entry_t* lru = &rfs_cache[0];

    for (size_t i = 0; i < RFS_CACHE_NUM_ENTRIES; i++)
    {
        // Return first empty slot
        if (!rfs_cache[i].valid)
        {
            return &rfs_cache[i];
        }
        // Track least recently used
        if (rfs_cache[i].age < lru->age)
        {
            lru = &rfs_cache[i];
        }
    }
    return lru;
}

// Add or update entry in cache
static void rfs_cache_put(uint8_t drive, uint8_t track, uint8_t sector, const uint8_t* data)
{
    // Check if already in cache
    rfs_cache_entry_t* entry = rfs_cache_find(drive, track, sector);

    if (!entry)
    {
        // Find slot for new entry (LRU eviction)
        entry = rfs_cache_find_lru();
    }

    entry->drive = drive;
    entry->track = track;
    entry->sector = sector;
    entry->valid = 1;
    entry->age = ++rfs_cache_age_counter;
    memcpy(entry->data, data, RFS_SECTOR_SIZE);
}

// Get cached sector data (returns true if hit)
static bool rfs_cache_get(uint8_t drive, uint8_t track, uint8_t sector, uint8_t* data)
{
    rfs_cache_entry_t* entry = rfs_cache_find(drive, track, sector);

    if (entry)
    {
        memcpy(data, entry->data, RFS_SECTOR_SIZE);
        rfs_cache_hits++;
        return true;
    }

    rfs_cache_misses++;
    return false;
}

// Compare data with cached version (returns true if identical)
static bool rfs_cache_compare(uint8_t drive, uint8_t track, uint8_t sector, const uint8_t* data)
{
    rfs_cache_entry_t* entry = rfs_cache_find(drive, track, sector);

    if (entry)
    {
        return memcmp(entry->data, data, RFS_SECTOR_SIZE) == 0;
    }
    return false; // Not in cache = can't compare = must write
}

// Initialize cache
static void rfs_cache_init(void)
{
    memset(rfs_cache, 0, sizeof(rfs_cache));
    rfs_cache_age_counter = 0;
    rfs_cache_hits = 0;
    rfs_cache_misses = 0;
    printf("[RFS_CACHE] Initialized %u entries (%u KB)\n", (unsigned)RFS_CACHE_NUM_ENTRIES,
           (unsigned)(RFS_CACHE_SIZE_BYTES / 1024));
}

// Print cache statistics
void rfs_cache_print_stats(void)
{
    uint32_t total = rfs_cache_hits + rfs_cache_misses;
    if (total > 0)
    {
        printf("[RFS_CACHE] Hits: %u, Misses: %u, Hit rate: %u%%\n", (unsigned)rfs_cache_hits,
               (unsigned)rfs_cache_misses, (unsigned)((rfs_cache_hits * 100) / total));
    }
}

// Periodic stats reporting
#define RFS_CACHE_STATS_INTERVAL_MS 30000 // Report every 30 seconds
static volatile bool rfs_cache_stats_pending = false;
static struct repeating_timer rfs_cache_stats_timer;

static bool rfs_cache_stats_timer_callback(struct repeating_timer* t)
{
    (void)t;
    rfs_cache_stats_pending = true;
    return true; // Keep timer running
}

// Check and print stats if timer fired (call from poll)
static void rfs_cache_check_stats(void)
{
    if (rfs_cache_stats_pending)
    {
        rfs_cache_stats_pending = false;
        uint32_t total = rfs_cache_hits + rfs_cache_misses;
        if (total > 0)
        {
            printf("[RFS_CACHE] Stats - Hits: %u, Misses: %u, Rate: %u%%, Write skips: %u\n", (unsigned)rfs_cache_hits,
                   (unsigned)rfs_cache_misses, (unsigned)((rfs_cache_hits * 100) / total),
                   (unsigned)rfs_cache_write_skips);
        }
    }
}

// Forward declarations
static err_t rfs_tcp_connected_cb(void* arg, struct tcp_pcb* tpcb, err_t err);
static err_t rfs_tcp_recv_cb(void* arg, struct tcp_pcb* tpcb, struct pbuf* p, err_t err);
static void rfs_tcp_err_cb(void* arg, err_t err);
static err_t rfs_tcp_sent_cb(void* arg, struct tcp_pcb* tpcb, u16_t len);

static void rfs_start_connect(void);
static void rfs_send_init(void);
static err_t rfs_send_read_request(const rfs_request_t* req);
static err_t rfs_send_write_request(const rfs_request_t* req);
static void rfs_handle_response(void);
static void rfs_set_error(const char* msg);
static void rfs_attempt_reconnect(void);

// ============================================================================
// Initialization
// ============================================================================

void rfs_client_init(void)
{
    // Initialize queues
    queue_init(&rfs_outbound_queue, sizeof(rfs_request_t), RFS_OUTBOUND_QUEUE_SIZE);
    queue_init(&rfs_inbound_queue, sizeof(rfs_response_t), RFS_INBOUND_QUEUE_SIZE);

    // Initialize state
    memset(&client, 0, sizeof(client));
    client.state = RFS_STATE_DISCONNECTED;

    // Initialize cache
    rfs_cache_init();

    // Start periodic cache stats timer
    add_repeating_timer_ms(RFS_CACHE_STATS_INTERVAL_MS, rfs_cache_stats_timer_callback, NULL, &rfs_cache_stats_timer);
}

// ============================================================================
// Core 1 Poll Function
// ============================================================================

void rfs_client_poll(void)
{
    // Check if cache stats should be printed
    rfs_cache_check_stats();

    // Check for timeout on operations
    if (client.request_in_progress)
    {
        uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - client.operation_start_time;
        uint32_t timeout = (client.state == RFS_STATE_CONNECTING) ? RFS_CONNECT_TIMEOUT_MS : RFS_OPERATION_TIMEOUT_MS;

        if (elapsed > timeout)
        {
            printf("[RFS] Operation timeout\n");
            rfs_attempt_reconnect();
            return;
        }
    }

    // State machine
    switch (client.state)
    {
        case RFS_STATE_DISCONNECTED:
        {
            // Check for connect request
            rfs_request_t request;
            if (queue_try_peek(&rfs_outbound_queue, &request))
            {
                // Auto-connect on ANY request (Connect, Read, or Write)
                printf("[RFS] pending request op=%u, connecting...\n", request.op);

                // If it's explicitly a CONNECT request, we can consume it now.
                // For READ/WRITE, leave it in queue to be processed after connection.
                if (request.op == RFS_OP_CONNECT)
                {
                    queue_try_remove(&rfs_outbound_queue, &request);
                }

                rfs_start_connect();
            }
            break;
        }

        case RFS_STATE_CONNECTING:
            // Waiting for TCP connection callback
            break;

        case RFS_STATE_CONNECTED:
            // Send INIT command
            rfs_send_init();
            break;

        case RFS_STATE_INIT_SENT:
            // Waiting for INIT response
            break;

        case RFS_STATE_READY:
        {
            // Ready for operations - simple pop-and-send (strict request-response protocol)
            if (!client.request_in_progress)
            {
                rfs_request_t request;
                // Peek first to handle backpressure - only pop if send succeeds
                if (queue_try_peek(&rfs_outbound_queue, &request))
                {
                    err_t err = ERR_OK;
                    size_t expected = 0;

                    switch (request.op)
                    {
                        case RFS_OP_READ:
                            expected = 1 + RFS_SECTOR_SIZE; // status + data
                            err = rfs_send_read_request(&request);
                            break;

                        case RFS_OP_WRITE:
                            expected = 1; // status only
                            err = rfs_send_write_request(&request);
                            break;

                        default:
                            // Unknown op, just remove it
                            queue_try_remove(&rfs_outbound_queue, &request);
                            break;
                    }

                    if (err == ERR_OK)
                    {
                        // Success - now pop the request and set up for response
                        queue_try_remove(&rfs_outbound_queue, &request);
                        client.current_request = request;
                        client.expected_len = expected;
                        client.request_in_progress = true;
                        client.recv_len = 0;
                        client.operation_start_time = to_ms_since_boot(get_absolute_time());
                    }
                    else if (err == ERR_MEM)
                    {
                        // Backpressure: TCP buffer full, leave in queue, try again next poll
                        // (no action needed - request stays in queue)
                    }
                    else
                    {
                        // Actual error - trigger reconnect
                        queue_try_remove(&rfs_outbound_queue, &request);
                        client.current_request = request;
                        rfs_attempt_reconnect();
                    }
                }
            }
            break;
        }

        case RFS_STATE_RECONNECTING:
        {
            // Wait for reconnect delay, then attempt reconnect
            uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - client.reconnect_start_time;
            if (elapsed >= RFS_RECONNECT_DELAY_MS)
            {
                printf("[RFS] Reconnect attempt %d/%d\\n", client.retry_count, RFS_MAX_RETRIES);
                rfs_start_connect();
            }
            break;
        }

        case RFS_STATE_ERROR:
            // Fatal error state - no recovery (max retries exceeded)
            break;
    }
}

// ============================================================================
// TCP Connection
// ============================================================================

static void rfs_start_connect(void)
{
    printf("[RFS] Connecting to %s:%d\n", RFS_SERVER_IP, RFS_SERVER_PORT);

    // Parse IP address
    ip_addr_t server_addr;
    if (!ip4addr_aton(RFS_SERVER_IP, &server_addr))
    {
        rfs_set_error("Invalid server IP address");
        return;
    }

    // Create TCP PCB
    client.pcb = tcp_new();
    if (client.pcb == NULL)
    {
        rfs_set_error("Failed to create TCP PCB");
        return;
    }

    // Set callbacks
    tcp_arg(client.pcb, NULL);
    tcp_err(client.pcb, rfs_tcp_err_cb);
    tcp_recv(client.pcb, rfs_tcp_recv_cb);
    tcp_sent(client.pcb, rfs_tcp_sent_cb);

    // Start connection
    client.state = RFS_STATE_CONNECTING;
    client.request_in_progress = true;
    client.operation_start_time = to_ms_since_boot(get_absolute_time());

    // Disable Nagle's algorithm for lower latency on small packets
    tcp_nagle_disable(client.pcb);

    err_t err = tcp_connect(client.pcb, &server_addr, RFS_SERVER_PORT, rfs_tcp_connected_cb);
    if (err != ERR_OK)
    {
        rfs_set_error("TCP connect failed");
        return;
    }
}

static err_t rfs_tcp_connected_cb(void* arg, struct tcp_pcb* tpcb, err_t err)
{
    (void)arg;
    (void)tpcb;

    if (err != ERR_OK)
    {
        rfs_set_error("Connection failed");
        return err;
    }

    printf("[RFS] Connected to server\n");
    client.state = RFS_STATE_CONNECTED;
    client.request_in_progress = false;

    return ERR_OK;
}

static void rfs_tcp_err_cb(void* arg, err_t err)
{
    (void)arg;
    printf("[RFS] TCP error: %d\n", err);

    client.pcb = NULL; // PCB already freed by lwIP

    // Attempt to reconnect instead of fatal error
    if (client.request_in_progress)
    {
        rfs_attempt_reconnect();
    }
    else
    {
        // No pending request, just go to disconnected state
        client.state = RFS_STATE_DISCONNECTED;
    }
}

static err_t rfs_tcp_sent_cb(void* arg, struct tcp_pcb* tpcb, u16_t len)
{
    (void)arg;
    (void)tpcb;
    (void)len;
    return ERR_OK;
}

// ============================================================================
// TCP Receive
// ============================================================================

static err_t rfs_tcp_recv_cb(void* arg, struct tcp_pcb* tpcb, struct pbuf* p, err_t err)
{
    (void)arg;

    if (err != ERR_OK)
    {
        if (p != NULL)
            pbuf_free(p);
        rfs_set_error("Receive error");
        return err;
    }

    if (p == NULL)
    {
        // Connection closed by server
        rfs_set_error("Server closed connection");
        return ERR_OK;
    }

    // Copy data to receive buffer
    size_t copy_len = p->tot_len;
    if (client.recv_len + copy_len > RFS_RECV_BUF_SIZE)
    {
        copy_len = RFS_RECV_BUF_SIZE - client.recv_len;
    }

    pbuf_copy_partial(p, &client.recv_buf[client.recv_len], copy_len, 0);
    client.recv_len += copy_len;

    // ACK received data
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);

    // Check if we have complete response
    if (client.recv_len >= client.expected_len)
    {
        rfs_handle_response();
    }

    return ERR_OK;
}

// ============================================================================
// Protocol Commands
// ============================================================================

static void rfs_send_init(void)
{
    if (client.pcb == NULL)
        return;

    uint8_t cmd = RFS_CMD_INIT;
    err_t err = tcp_write(client.pcb, &cmd, 1, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK)
    {
        rfs_set_error("Failed to send INIT");
        return;
    }

    tcp_output(client.pcb);

    client.state = RFS_STATE_INIT_SENT;
    client.expected_len = 1; // status only
    client.recv_len = 0;
    client.request_in_progress = true;
    client.operation_start_time = to_ms_since_boot(get_absolute_time());
}

static err_t rfs_send_read_request(const rfs_request_t* req)
{
    if (client.pcb == NULL)
        return ERR_CONN;

    // Build request: cmd + drive + track + sector
    uint8_t buf[4];
    buf[0] = RFS_CMD_READ_SECTOR;
    buf[1] = req->drive;
    buf[2] = req->track;
    buf[3] = req->sector;

    err_t err = tcp_write(client.pcb, buf, 4, TCP_WRITE_FLAG_COPY);
    if (err == ERR_OK)
    {
        tcp_output(client.pcb);
    }
    return err;
}

static err_t rfs_send_write_request(const rfs_request_t* req)
{
    if (client.pcb == NULL)
        return ERR_CONN;

    // Build request in static buffer: cmd + drive + track + sector + data
    rfs_write_buf[0] = RFS_CMD_WRITE_SECTOR;
    rfs_write_buf[1] = req->drive;
    rfs_write_buf[2] = req->track;
    rfs_write_buf[3] = req->sector;
    memcpy(&rfs_write_buf[4], req->data, RFS_SECTOR_SIZE);

    err_t err = tcp_write(client.pcb, rfs_write_buf, 4 + RFS_SECTOR_SIZE, TCP_WRITE_FLAG_COPY);
    if (err == ERR_OK)
    {
        tcp_output(client.pcb);
    }
    return err;
}

static void rfs_handle_response(void)
{
    rfs_response_t response;

    if (client.state == RFS_STATE_INIT_SENT)
    {
        // INIT response
        response.op = RFS_OP_INIT;
        response.status = client.recv_buf[0];

        if (response.status == RFS_RESP_OK)
        {
            printf("[RFS] INIT OK, ready for operations\n");
            client.state = RFS_STATE_READY;
            client.retry_count = 0; // Reset retry counter on success

            // If we have a pending retry request, resend it now
            if (client.has_pending_retry)
            {
                printf("[RFS] ✓ Reconnected! Resending failed %s request...\n",
                       client.pending_retry_request.op == RFS_OP_READ ? "READ" : "WRITE");
                client.has_pending_retry = false;
                client.current_request = client.pending_retry_request;
                client.request_in_progress = true;
                client.recv_len = 0;
                client.operation_start_time = to_ms_since_boot(get_absolute_time());

                err_t err = ERR_OK;
                switch (client.current_request.op)
                {
                    case RFS_OP_READ:
                        client.expected_len = 1 + RFS_SECTOR_SIZE;
                        err = rfs_send_read_request(&client.current_request);
                        break;
                    case RFS_OP_WRITE:
                        client.expected_len = 1;
                        err = rfs_send_write_request(&client.current_request);
                        break;
                    default:
                        client.request_in_progress = false;
                        break;
                }

                if (err != ERR_OK)
                {
                    rfs_attempt_reconnect();
                }
                return; // Don't send INIT response to Core 0 during retry
            }
        }
        else
        {
            rfs_set_error("INIT failed");
            return;
        }

        // Queue response for Core 0 (only if not retry)
        queue_try_add(&rfs_inbound_queue, &response);
        client.request_in_progress = false;
    }
    else if (client.request_in_progress)
    {
        // READ or WRITE response
        response.op = client.current_request.op;
        response.status = client.recv_buf[0];

        if (response.op == RFS_OP_READ && response.status == RFS_RESP_OK)
        {
            memcpy(response.data, &client.recv_buf[1], RFS_SECTOR_SIZE);

            // Update cache with fetched data
            rfs_cache_put(client.current_request.drive, client.current_request.track, client.current_request.sector,
                          response.data);
        }

        // Queue response for Core 0
        queue_try_add(&rfs_inbound_queue, &response);
        client.request_in_progress = false;
        client.recv_len = 0;

        // Log recovery success if we just completed a retry
        if (client.retry_count > 0 && response.status == RFS_RESP_OK)
        {
            printf("[RFS] ✓ Recovery successful! Operation completed after %d retry(s)\n", client.retry_count);
            client.retry_count = 0;
        }
    }
}

static void rfs_set_error(const char* msg)
{
    printf("[RFS] FATAL ERROR: %s (retries exhausted)\n", msg);

    client.state = RFS_STATE_ERROR;
    client.request_in_progress = false;
    client.has_pending_retry = false;

    // Close connection if open
    if (client.pcb != NULL)
    {
        tcp_abort(client.pcb);
        client.pcb = NULL;
    }

    // Send error response to Core 0
    rfs_response_t response;
    memset(&response, 0, sizeof(response));
    response.op = client.current_request.op;
    response.status = RFS_RESP_ERROR;
    queue_try_add(&rfs_inbound_queue, &response);
}

static void rfs_attempt_reconnect(void)
{
    // Close existing connection
    if (client.pcb != NULL)
    {
        tcp_abort(client.pcb);
        client.pcb = NULL;
    }

    client.retry_count++;

    if (client.retry_count > RFS_MAX_RETRIES)
    {
        rfs_set_error("Max retries exceeded");
        return;
    }

    printf("[RFS] Connection lost, will retry (%d/%d)...\n", client.retry_count, RFS_MAX_RETRIES);

    // Save current request for retry
    client.has_pending_retry = true;
    client.pending_retry_request = client.current_request;
    client.request_in_progress = false;

    // Enter reconnecting state with delay
    client.state = RFS_STATE_RECONNECTING;
    client.reconnect_start_time = to_ms_since_boot(get_absolute_time());
}

// ============================================================================
// Core 0 API
// ============================================================================

bool rfs_client_is_ready(void)
{
    return client.state == RFS_STATE_READY;
}

bool rfs_client_has_error(void)
{
    return client.state == RFS_STATE_ERROR;
}

bool rfs_request_connect(void)
{
    rfs_request_t request = {0};
    request.op = RFS_OP_CONNECT;

    return queue_try_add(&rfs_outbound_queue, &request);
}

bool rfs_request_read(uint8_t drive, uint8_t track, uint8_t sector)
{
    // Check cache first - if hit, immediately queue response (no network)
    rfs_response_t cached_response;
    if (rfs_cache_get(drive, track, sector, cached_response.data))
    {
        cached_response.op = RFS_OP_READ;
        cached_response.status = RFS_RESP_OK;
        return queue_try_add(&rfs_inbound_queue, &cached_response);
    }

    // Cache miss - queue network request
    rfs_request_t request = {.op = RFS_OP_READ, .drive = drive, .track = track, .sector = sector};
    return queue_try_add(&rfs_outbound_queue, &request);
}

bool rfs_request_write(uint8_t drive, uint8_t track, uint8_t sector, const uint8_t* data)
{
    // Check if data is identical to cached version (skip redundant write)
    if (rfs_cache_compare(drive, track, sector, data))
    {
        rfs_cache_write_skips++;
        // Data unchanged - send immediate success response without network
        rfs_response_t response = {.op = RFS_OP_WRITE, .status = RFS_RESP_OK};
        return queue_try_add(&rfs_inbound_queue, &response);
    }

    // Update cache (write-through: cache updated immediately, request still goes to server)
    rfs_cache_put(drive, track, sector, data);

    rfs_request_t request = {.op = RFS_OP_WRITE, .drive = drive, .track = track, .sector = sector};
    memcpy(request.data, data, RFS_SECTOR_SIZE);

    return queue_try_add(&rfs_outbound_queue, &request);
}

bool rfs_get_response(rfs_response_t* response)
{
    return queue_try_remove(&rfs_inbound_queue, response);
}

bool rfs_request_pending(void)
{
    return client.request_in_progress || !queue_is_empty(&rfs_outbound_queue);
}

#else // !CYW43_WL_GPIO_LED_PIN - Stub implementations for non-WiFi boards

void rfs_client_init(void) {}
void rfs_client_poll(void) {}
bool rfs_client_is_ready(void)
{
    return false;
}
bool rfs_client_has_error(void)
{
    return true;
}
bool rfs_request_connect(void)
{
    return false;
}
bool rfs_request_read(uint8_t drive, uint8_t track, uint8_t sector)
{
    (void)drive;
    (void)track;
    (void)sector;
    return false;
}
bool rfs_request_write(uint8_t drive, uint8_t track, uint8_t sector, const uint8_t* data)
{
    (void)drive;
    (void)track;
    (void)sector;
    (void)data;
    return false;
}
bool rfs_get_response(rfs_response_t* response)
{
    (void)response;
    return false;
}
bool rfs_request_pending(void)
{
    return false;
}

#endif // CYW43_WL_GPIO_LED_PIN
