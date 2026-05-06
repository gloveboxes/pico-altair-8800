/**
 * File Transfer Port Driver for Altair 8800 Emulator
 * 
 * Architecture: pico_cyw43_arch_lwip_threadsafe_background
 * 
 * Key design principles:
 * 1. lwIP callbacks run in INTERRUPT context (lock implicitly held)
 * 2. Poll function runs in THREAD context (needs explicit lock for TCP calls)
 * 3. Callbacks only SET STATE - never call tcp_abort() or complex logic
 * 4. Poll loop handles ALL TCP operations and cleanup
 * 5. Inter-core communication via lock-free queues (Core 0 <-> Core 1)
 */

#include "files_io.h"

#include "pico/stdlib.h"

// File transfer is only available on WiFi-enabled boards
#if defined(CYW43_WL_GPIO_LED_PIN)

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "lwip/err.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "pico/cyw43_arch.h"
#include "pico/util/queue.h"

// =============================================================================
// Configuration
// =============================================================================

#ifndef FT_SERVER_PORT
#define FT_SERVER_PORT 8090
#endif

#define FT_RECV_BUF_SIZE     (1 + 1 + FT_CHUNK_SIZE)
#define FT_CONNECT_TIMEOUT_MS 10000
#define FT_OPERATION_TIMEOUT_MS 15000
#define FT_MAX_RETRIES        20
#define FT_RECONNECT_DELAY_MS 500

// Protocol commands
#define FT_PROTO_GET_CHUNK    0x02
#define FT_PROTO_CLOSE        0x03

// Protocol responses
#define FT_PROTO_RESP_OK      0x00
#define FT_PROTO_RESP_EOF     0x01
#define FT_PROTO_RESP_ERROR   0xFF

// =============================================================================
// Types
// =============================================================================

typedef enum {
    FT_REQ_GET_CHUNK = 0,
    FT_REQ_CLOSE
} ft_request_type_t;

typedef struct {
    ft_request_type_t type;
    uint32_t offset;
    uint8_t data[257];  // filename (null-terminated)
} ft_request_t;

typedef struct {
    ft_status_t status;
    uint8_t data[FT_CHUNK_SIZE];
    size_t len;
    uint8_t count;
    bool has_count;
} ft_response_t;

// Simplified state machine - fewer states, clearer transitions
typedef enum {
    FT_STATE_IDLE = 0,       // No connection, no pending work
    FT_STATE_CONNECTING,     // tcp_connect() called, waiting for callback
    FT_STATE_CONNECTED,      // Connected, ready to send requests
    FT_STATE_WAITING,        // Request sent, waiting for response
    FT_STATE_ERROR           // Fatal error, needs manual reset
} ft_state_t;

// =============================================================================
// State
// =============================================================================

// Inter-core queues
static queue_t ft_request_queue;
static queue_t ft_response_queue;

// TCP client state (Core 1 only)
static struct {
    ft_state_t state;
    struct tcp_pcb* pcb;
    
    // Current operation
    ft_request_t current_request;
    
    // Receive buffer
    uint8_t recv_buf[FT_RECV_BUF_SIZE];
    size_t recv_len;
    size_t expected_len;
    
    // Timing
    uint32_t op_start_time;
    uint32_t reconnect_time;
    uint8_t retry_count;
    
    // Flags set by callbacks (volatile for interrupt safety)
    volatile bool connected;        // Set by connect callback
    volatile bool disconnected;     // Set by error callback  
    volatile bool data_ready;       // Set by recv callback when complete
    volatile const char* error_msg; // Set by callbacks on error
} client;

// Port state (Core 0 only)
static struct {
    char filename[128];
    size_t filename_idx;
    uint8_t chunk_buffer[FT_CHUNK_SIZE + 1];
    size_t chunk_len;
    size_t chunk_position;
    uint32_t file_offset;
    ft_status_t status;
} port_state;

// =============================================================================
// Forward Declarations
// =============================================================================

static void ft_do_connect(void);
static void ft_do_send_request(void);
static void ft_do_cleanup(void);
static void ft_process_response(void);
static void ft_queue_error_response(void);

// =============================================================================
// Initialization
// =============================================================================

void files_io_init(void)
{
    queue_init(&ft_request_queue, sizeof(ft_request_t), 1);
    queue_init(&ft_response_queue, sizeof(ft_response_t), 1);
    
    memset(&client, 0, sizeof(client));
    client.state = FT_STATE_IDLE;
    
    memset(&port_state, 0, sizeof(port_state));
    port_state.status = FT_STATUS_IDLE;
    
    printf("[FT] File transfer initialized (threadsafe_background)\n");
}

// =============================================================================
// Core 0: Port Handlers (unchanged - these are correct)
// =============================================================================

static size_t files_output_command(uint8_t data)
{
    ft_request_t request;
    
    switch ((ft_command_t)data)
    {
        case FT_CMD_NOP:
        case FT_CMD_FILENAME_CHAR:
            break;
            
        case FT_CMD_REQUEST_CHUNK:
            if (port_state.chunk_position < port_state.chunk_len)
                break;  // Still have data
                
            request.type = FT_REQ_GET_CHUNK;
            request.offset = port_state.file_offset;
            memcpy(request.data, port_state.filename, strlen(port_state.filename) + 1);
            
            if (queue_try_add(&ft_request_queue, &request)) {
                port_state.chunk_len = 0;
                port_state.chunk_position = 0;
                port_state.status = FT_STATUS_BUSY;
            } else {
                port_state.status = FT_STATUS_ERROR;
            }
            break;
            
        case FT_CMD_CLOSE:
            request.type = FT_REQ_CLOSE;
            memcpy(request.data, port_state.filename, strlen(port_state.filename) + 1);
            queue_try_add(&ft_request_queue, &request);
            port_state.status = FT_STATUS_IDLE;
            break;
            
        default:
            break;
    }
    return 0;
}

static void files_output_data(uint8_t data)
{
    if (data == 0) {
        port_state.filename[port_state.filename_idx] = '\0';
        port_state.chunk_len = 0;
        port_state.chunk_position = 0;
        port_state.file_offset = 0;
        port_state.filename_idx = 0;
        port_state.status = FT_STATUS_IDLE;
    } else if (port_state.filename_idx < sizeof(port_state.filename) - 1) {
        port_state.filename[port_state.filename_idx++] = (char)data;
    } else {
        port_state.status = FT_STATUS_ERROR;
        port_state.filename_idx = 0;
    }
}

static uint8_t files_input_status(void)
{
    if (port_state.chunk_len == 0 || port_state.chunk_position >= port_state.chunk_len) {
        ft_response_t response;
        if (queue_try_remove(&ft_response_queue, &response)) {
            if (response.has_count) {
                port_state.chunk_buffer[0] = response.count;
                if (response.len > 0)
                    memcpy(&port_state.chunk_buffer[1], response.data, response.len);
                port_state.chunk_len = response.len + 1;
                port_state.chunk_position = 0;
                port_state.file_offset += response.len;
            }
            port_state.status = response.status;
        }
    }
    
    if (port_state.chunk_position < port_state.chunk_len && port_state.status != FT_STATUS_ERROR)
        return FT_STATUS_DATAREADY;
        
    return port_state.status;
}

static uint8_t files_input_data(void)
{
    if (port_state.chunk_position < port_state.chunk_len)
        return port_state.chunk_buffer[port_state.chunk_position++];
    return 0x00;
}

size_t files_output(int port, uint8_t data, char* buffer, size_t buffer_length)
{
    (void)buffer; (void)buffer_length;
    if (port == 60) return files_output_command(data);
    if (port == 61) { files_output_data(data); return 0; }
    return 0;
}

uint8_t files_input(uint8_t port)
{
    if (port == 60) return files_input_status();
    if (port == 61) return files_input_data();
    return 0x00;
}

// =============================================================================
// TCP Callbacks (Interrupt Context - Keep Minimal!)
// 
// These run with the lwIP lock held. They should ONLY:
// - Copy data to buffers
// - Set flags
// - Return appropriate error codes
// =============================================================================

static err_t ft_tcp_connected_cb(void* arg, struct tcp_pcb* tpcb, err_t err)
{
    (void)arg; (void)tpcb;
    
    if (err == ERR_OK) {
        client.connected = true;
    } else {
        client.error_msg = "Connection failed";
        client.disconnected = true;
    }
    return ERR_OK;
}

static void ft_tcp_err_cb(void* arg, err_t err)
{
    (void)arg; (void)err;
    // PCB is already freed by lwIP when this is called
    client.pcb = NULL;
    client.disconnected = true;
    client.error_msg = "TCP error";
}

static err_t ft_tcp_recv_cb(void* arg, struct tcp_pcb* tpcb, struct pbuf* p, err_t err)
{
    (void)arg;
    
    if (err != ERR_OK || p == NULL) {
        if (p) pbuf_free(p);
        client.disconnected = true;
        client.error_msg = p ? "Receive error" : "Server closed";
        return ERR_OK;
    }
    
    // Copy to receive buffer
    size_t copy_len = p->tot_len;
    if (client.recv_len + copy_len <= FT_RECV_BUF_SIZE) {
        pbuf_copy_partial(p, &client.recv_buf[client.recv_len], copy_len, 0);
        client.recv_len += copy_len;
    }
    
    // ACK the data
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    
    // Check if we have enough data to determine expected length
    if (client.expected_len == 2 && client.recv_len >= 2) {
        uint8_t status = client.recv_buf[0];
        uint8_t count = client.recv_buf[1];
        if (status == FT_PROTO_RESP_OK || status == FT_PROTO_RESP_EOF) {
            client.expected_len = 2 + (count == 0 ? FT_CHUNK_SIZE : count);
        }
    }
    
    // Signal if response complete
    if (client.recv_len >= client.expected_len) {
        client.data_ready = true;
    }
    
    return ERR_OK;
}

static err_t ft_tcp_sent_cb(void* arg, struct tcp_pcb* tpcb, u16_t len)
{
    (void)arg; (void)tpcb; (void)len;
    return ERR_OK;
}

// =============================================================================
// Core 1: Poll Function (Thread Context)
// 
// This is the main state machine. All TCP operations happen here with
// explicit locking. Callbacks only set flags that we check here.
// =============================================================================

void ft_client_poll(void)
{
    // Handle flags set by callbacks
    if (client.disconnected) {
        client.disconnected = false;
        if (client.error_msg) {
            printf("[FT] %s\n", client.error_msg);
            client.error_msg = NULL;
        }
        ft_do_cleanup();
        
        // Retry if we have a pending request
        if (client.state == FT_STATE_WAITING || client.state == FT_STATE_CONNECTING) {
            if (++client.retry_count <= FT_MAX_RETRIES) {
                printf("[FT] Will retry (%d/%d)\n", client.retry_count, FT_MAX_RETRIES);
                client.reconnect_time = to_ms_since_boot(get_absolute_time());
                client.state = FT_STATE_IDLE;  // Will reconnect
            } else {
                printf("[FT] Max retries exceeded\n");
                client.state = FT_STATE_ERROR;
                ft_queue_error_response();
            }
        } else {
            client.state = FT_STATE_IDLE;
        }
        return;
    }
    
    if (client.connected) {
        client.connected = false;
        printf("[FT] Connected\n");
        client.state = FT_STATE_CONNECTED;
        client.retry_count = 0;
    }
    
    if (client.data_ready) {
        client.data_ready = false;
        ft_process_response();
        client.state = FT_STATE_CONNECTED;
    }
    
    // Check timeouts
    if (client.state == FT_STATE_CONNECTING || client.state == FT_STATE_WAITING) {
        uint32_t timeout = (client.state == FT_STATE_CONNECTING) 
            ? FT_CONNECT_TIMEOUT_MS : FT_OPERATION_TIMEOUT_MS;
        uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - client.op_start_time;
        
        if (elapsed > timeout) {
            printf("[FT] Timeout\n");
            ft_do_cleanup();
            client.disconnected = true;  // Trigger retry logic
            return;
        }
    }
    
    // State machine
    switch (client.state)
    {
        case FT_STATE_IDLE: {
            // Check for pending requests
            ft_request_t request;
            if (queue_try_peek(&ft_request_queue, &request)) {
                // Check reconnect delay
                if (client.retry_count > 0) {
                    uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - client.reconnect_time;
                    if (elapsed < FT_RECONNECT_DELAY_MS) break;
                }
                printf("[FT] Connecting...\n");
                ft_do_connect();
            }
            break;
        }
        
        case FT_STATE_CONNECTED: {
            ft_request_t request;
            if (queue_try_remove(&ft_request_queue, &request)) {
                client.current_request = request;
                ft_do_send_request();
            }
            break;
        }
        
        case FT_STATE_CONNECTING:
        case FT_STATE_WAITING:
            // Waiting for callback
            break;
            
        case FT_STATE_ERROR:
            // Stuck until reset
            break;
    }
}

// =============================================================================
// TCP Operations (Thread Context - require explicit locking)
// =============================================================================

static void ft_do_connect(void)
{
    const char* server_ip = config_get_rfs_ip();
    if (server_ip[0] == '\0') {
        printf("[FT] No server IP configured\n");
        client.state = FT_STATE_ERROR;
        ft_queue_error_response();
        return;
    }
    
    ip_addr_t addr;
    if (!ip4addr_aton(server_ip, &addr)) {
        printf("[FT] Invalid IP\n");
        client.state = FT_STATE_ERROR;
        ft_queue_error_response();
        return;
    }
    
    cyw43_arch_lwip_begin();
    
    client.pcb = tcp_new();
    if (!client.pcb) {
        cyw43_arch_lwip_end();
        printf("[FT] PCB alloc failed\n");
        client.state = FT_STATE_ERROR;
        ft_queue_error_response();
        return;
    }
    
    tcp_arg(client.pcb, NULL);
    tcp_err(client.pcb, ft_tcp_err_cb);
    tcp_recv(client.pcb, ft_tcp_recv_cb);
    tcp_sent(client.pcb, ft_tcp_sent_cb);
    tcp_nagle_disable(client.pcb);
    
    err_t err = tcp_connect(client.pcb, &addr, FT_SERVER_PORT, ft_tcp_connected_cb);
    
    if (err != ERR_OK) {
        tcp_abort(client.pcb);
        client.pcb = NULL;
        cyw43_arch_lwip_end();
        client.disconnected = true;
        return;
    }
    
    cyw43_arch_lwip_end();
    
    client.state = FT_STATE_CONNECTING;
    client.op_start_time = to_ms_since_boot(get_absolute_time());
}

static void ft_do_send_request(void)
{
    uint8_t buf[260];
    size_t len = 0;
    
    if (client.current_request.type == FT_REQ_GET_CHUNK) {
        buf[0] = FT_PROTO_GET_CHUNK;
        buf[1] = client.current_request.offset & 0xFF;
        buf[2] = (client.current_request.offset >> 8) & 0xFF;
        buf[3] = (client.current_request.offset >> 16) & 0xFF;
        buf[4] = (client.current_request.offset >> 24) & 0xFF;
        size_t name_len = strlen((char*)client.current_request.data);
        memcpy(&buf[5], client.current_request.data, name_len + 1);
        len = 5 + name_len + 1;
        client.expected_len = 2;  // Will be updated when we see count byte
    } else {
        buf[0] = FT_PROTO_CLOSE;
        size_t name_len = strlen((char*)client.current_request.data);
        memcpy(&buf[1], client.current_request.data, name_len + 1);
        len = 1 + name_len + 1;
        client.expected_len = 1;
        printf("[FT] Closing file\n");
    }
    
    client.recv_len = 0;
    
    cyw43_arch_lwip_begin();
    err_t err = tcp_write(client.pcb, buf, len, TCP_WRITE_FLAG_COPY);
    if (err == ERR_OK) {
        tcp_output(client.pcb);
    }
    cyw43_arch_lwip_end();
    
    if (err != ERR_OK) {
        printf("[FT] Send failed: %d\n", err);
        client.disconnected = true;
        return;
    }
    
    client.state = FT_STATE_WAITING;
    client.op_start_time = to_ms_since_boot(get_absolute_time());
}

static void ft_do_cleanup(void)
{
    if (client.pcb) {
        cyw43_arch_lwip_begin();
        tcp_abort(client.pcb);
        cyw43_arch_lwip_end();
        client.pcb = NULL;
    }
    client.recv_len = 0;
}

static void ft_process_response(void)
{
    ft_response_t response = {0};
    uint8_t status = client.recv_buf[0];
    
    if (client.current_request.type == FT_REQ_GET_CHUNK) {
        response.status = (status == FT_PROTO_RESP_OK) ? FT_STATUS_DATAREADY :
                         (status == FT_PROTO_RESP_EOF) ? FT_STATUS_EOF : FT_STATUS_ERROR;
        
        if (status == FT_PROTO_RESP_OK || status == FT_PROTO_RESP_EOF) {
            uint8_t count = client.recv_buf[1];
            response.len = (count == 0) ? 256 : count;
            response.count = count;
            response.has_count = true;
            memcpy(response.data, &client.recv_buf[2], response.len);
        }
    } else {
        response.status = (status == FT_PROTO_RESP_ERROR) ? FT_STATUS_ERROR : FT_STATUS_IDLE;
    }
    
    // Force-add (replace stale if needed)
    if (!queue_try_add(&ft_response_queue, &response)) {
        ft_response_t dummy;
        queue_try_remove(&ft_response_queue, &dummy);
        queue_try_add(&ft_response_queue, &response);
    }
    
    // Handle coalesced responses
    if (client.recv_len > client.expected_len) {
        size_t remaining = client.recv_len - client.expected_len;
        memmove(client.recv_buf, &client.recv_buf[client.expected_len], remaining);
        client.recv_len = remaining;
    } else {
        client.recv_len = 0;
    }
}

static void ft_queue_error_response(void)
{
    ft_response_t response = {0};
    response.status = FT_STATUS_ERROR;
    
    if (!queue_try_add(&ft_response_queue, &response)) {
        ft_response_t dummy;
        queue_try_remove(&ft_response_queue, &dummy);
        queue_try_add(&ft_response_queue, &response);
    }
    
    // Drain request queue
    ft_request_t req;
    while (queue_try_remove(&ft_request_queue, &req)) {}
}

#else // Non-WiFi stubs

#include <stddef.h>
#include <stdint.h>

void files_io_init(void) {}

size_t files_output(int port, uint8_t data, char* buffer, size_t buffer_length)
{
    (void)port; (void)data; (void)buffer; (void)buffer_length;
    return 0;
}

uint8_t files_input(uint8_t port)
{
    (void)port;
    return 0xFF;
}

void ft_client_poll(void) {}

#endif
