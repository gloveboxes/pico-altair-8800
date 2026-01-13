#include "files_io.h"

#include "pico/stdlib.h" // Must be included before WiFi check to get board definitions

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

// Server port
#ifndef FT_SERVER_PORT
#define FT_SERVER_PORT 8090
#endif

// Protocol commands (to server)
#define FT_PROTO_GET_CHUNK 0x02
#define FT_PROTO_CLOSE 0x03

// Protocol responses (from server)
// Wire format: [status:1][count:1][data:count] (variable length)
//
// RESP_OK (0x00):   1-256 valid bytes, count_byte=0 encodes 256
// RESP_EOF (0x01):  1-256 valid bytes, count_byte=0 encodes 256 (final chunk)
// RESP_ERROR (0xFF): status+count only, no data payload
//
// Protocol flow: Request → Response → Consume → Request (synchronous)
#define FT_PROTO_RESP_OK 0x00
#define FT_PROTO_RESP_EOF 0x01
#define FT_PROTO_RESP_ERROR 0xFF

// Queue sizes
// Size 1 is sufficient: Core 0 blocks between requests, consuming response N before sending request N+1
#define FT_REQUEST_QUEUE_SIZE 1
#define FT_RESPONSE_QUEUE_SIZE 1

// Receive buffer size (max status + count + chunk data)
#define FT_RECV_BUF_SIZE (1 + 1 + FT_CHUNK_SIZE)

// Connection timeout (ms)
#define FT_CONNECT_TIMEOUT_MS 10000
#define FT_OPERATION_TIMEOUT_MS 15000
#define FT_MAX_RETRIES 3
#define FT_RECONNECT_DELAY_MS 500

// Forward declarations
static void files_output_data(uint8_t data);

// Request types for inter-core queue
typedef enum
{
    FT_REQ_CONNECT = 0,
    FT_REQ_GET_CHUNK,
    FT_REQ_CLOSE
} ft_request_type_t;

// Request structure (Core 0 -> Core 1)
typedef struct
{
    ft_request_type_t type;
    uint32_t offset;    // file offset for GET_CHUNK (stateless protocol)
    uint8_t data[257];  // filename (null-terminated) or empty
} ft_request_t;

// Response structure (Core 1 -> Core 0)
typedef struct
{
    ft_status_t status;
    uint8_t data[FT_CHUNK_SIZE];
    size_t len; // actual bytes in this chunk (0-256)
    uint8_t count; // raw count byte (0 encodes 256)
    bool has_count;
    uint16_t session_id; // to detect stale responses
} ft_response_t;

// Client state machine
typedef enum
{
    FT_STATE_DISCONNECTED = 0,
    FT_STATE_CONNECTING,
    FT_STATE_CONNECTED,
    FT_STATE_READY,
    FT_STATE_WAITING_RESPONSE,
    FT_STATE_RECONNECTING,
    FT_STATE_ERROR
} ft_client_state_t;

// Queues for inter-core communication
static queue_t ft_request_queue;  // Core 0 -> Core 1
static queue_t ft_response_queue; // Core 1 -> Core 0

// Client state
static struct
{
    ft_client_state_t state;
    struct tcp_pcb* pcb;

    // Current request
    ft_request_t current_request;
    bool request_in_progress;

    // Receive buffer
    uint8_t recv_buf[FT_RECV_BUF_SIZE];
    size_t recv_len;
    size_t expected_len;

    // Timing
    uint32_t operation_start_time;

    // Retry state
    uint8_t retry_count;
    uint32_t reconnect_start_time;

    // Session tracking to prevent stale responses
    uint16_t session_id;
} ft_client;

// Port state (Core 0)
static struct
{
    char filename[128];
    size_t filename_idx;

    // Current chunk being read by Altair (count byte + data)
    uint8_t chunk_buffer[FT_CHUNK_SIZE + 1];
    size_t chunk_len;      // bytes available in this chunk
    size_t chunk_position; // current read position

    // File position tracking for stateless protocol
    uint32_t file_offset;

    ft_status_t status;
} port_state;

// Forward declarations
static err_t ft_tcp_connected_cb(void* arg, struct tcp_pcb* tpcb, err_t err);
static err_t ft_tcp_recv_cb(void* arg, struct tcp_pcb* tpcb, struct pbuf* p, err_t err);
static void ft_tcp_err_cb(void* arg, err_t err);
static err_t ft_tcp_sent_cb(void* arg, struct tcp_pcb* tpcb, u16_t len);

static void ft_start_connect(void);
static void ft_handle_response(void);
static void ft_set_error(const char* msg);
static void ft_attempt_reconnect(void);

// ============================================================================
// Initialization
// ============================================================================

void files_io_init(void)
{
    // Initialize queues
    queue_init(&ft_request_queue, sizeof(ft_request_t), FT_REQUEST_QUEUE_SIZE);
    queue_init(&ft_response_queue, sizeof(ft_response_t), FT_RESPONSE_QUEUE_SIZE);

    // Initialize client state
    memset(&ft_client, 0, sizeof(ft_client));
    ft_client.state = FT_STATE_DISCONNECTED;

    // Initialize port state
    memset(&port_state, 0, sizeof(port_state));
    port_state.status = FT_STATUS_IDLE;

    printf("[FT] File transfer port driver initialized (stateless protocol v3)\n");
}

// ============================================================================
// Core 0: Port Handlers
// ============================================================================

size_t files_output(int port, uint8_t data, char* buffer, size_t buffer_length)
{
    (void)buffer;
    (void)buffer_length;

    if (port == 61)
    {
        files_output_data(data);
        return 0;
    }

    if (port != 60)
        return 0;

    ft_request_t request;
    memset(&request, 0, sizeof(request));

    switch ((ft_command_t)data)
    {
        case FT_CMD_NOP:
            break;

        case FT_CMD_FILENAME_CHAR:
            // This command expects the character to follow via another OUT
            // Actually, we need to handle this differently - the character comes with the command
            // Let's use port 61 for sending filename characters
            break;

        case FT_CMD_REQUEST_CHUNK:
            // If we still have data in buffer, don't request more
            if (port_state.chunk_len > 0 && port_state.chunk_position < port_state.chunk_len)
            {
                break;
            }

            // Request next chunk from server (stateless: send offset + filename)
            request.type = FT_REQ_GET_CHUNK;
            request.offset = port_state.file_offset;
            strncpy((char*)request.data, port_state.filename, sizeof(request.data) - 1);

            // Reset state BEFORE queuing to prevent race where polling sees old state
            port_state.chunk_len = 0;
            port_state.chunk_position = 0;
            port_state.status = FT_STATUS_BUSY;

            if (!queue_try_add(&ft_request_queue, &request))
            {
                port_state.status = FT_STATUS_ERROR;
            }
            break;

        case FT_CMD_CLOSE:
            // Close file and evict from cache
            request.type = FT_REQ_CLOSE;
            strncpy((char*)request.data, port_state.filename, sizeof(request.data) - 1);
            queue_try_add(&ft_request_queue, &request);
            port_state.status = FT_STATUS_IDLE;
            break;

        default:
            break;
    }

    return 0;
}

// Special handler for filename/data output on port 61
static void files_output_data(uint8_t data)
{
    if (data == 0)
    {
        // Null terminator - filename complete
        port_state.filename[port_state.filename_idx] = '\0';

        // Reset state for new file
        port_state.chunk_len = 0;
        port_state.chunk_position = 0;
        port_state.file_offset = 0;
        port_state.filename_idx = 0;  // Reset for next filename
        port_state.status = FT_STATUS_IDLE;

        // Note: In stateless protocol, we don't send filename to server here.
        // The filename will be sent with each GET_CHUNK request.
    }
    else if (port_state.filename_idx < sizeof(port_state.filename) - 1)
    {
        port_state.filename[port_state.filename_idx++] = (char)data;
    }
    else
    {
        // Filename buffer overflow - reject and signal error
        printf("[FT] ERROR: Filename too long (max %zu chars)\n", sizeof(port_state.filename) - 1);
        port_state.status = FT_STATUS_ERROR;
        port_state.filename_idx = 0;
        memset(port_state.filename, 0, sizeof(port_state.filename));
    }
}

uint8_t files_input(uint8_t port)
{
    if (port == 60)
    {
        // Status port
        // Check for new response from Core 1
        if (port_state.chunk_len == 0 || port_state.chunk_position >= port_state.chunk_len)
        {
            ft_response_t response;
            if (queue_try_remove(&ft_response_queue, &response))
            {
                // Verify response matches current session (discard stale responses)
                if (response.session_id == ft_client.session_id)
                {
                    if (response.has_count)
                    {
                        port_state.chunk_buffer[0] = response.count;
                        if (response.len > 0)
                        {
                            memcpy(&port_state.chunk_buffer[1], response.data, response.len);
                        }
                        port_state.chunk_len = response.len + 1;
                        port_state.chunk_position = 0;
                        
                        // Update file offset for stateless protocol
                        port_state.file_offset += response.len;
                    }
                    else
                    {
                        port_state.chunk_len = 0;
                        port_state.chunk_position = 0;
                    }
                    port_state.status = response.status;
                }
                // Silently discard stale responses from previous session
            }
        }

        // If data is available in the buffer, always report DATAREADY
        // This prevents the CP/M app from seeing EOF before reading the last chunk
        if (port_state.chunk_position < port_state.chunk_len && port_state.status != FT_STATUS_ERROR)
        {
            return FT_STATUS_DATAREADY;
        }

        return port_state.status;
    }
    else if (port == 61)
    {
        // Data port - read byte from chunk
        if (port_state.chunk_position < port_state.chunk_len)
        {
            uint8_t byte = port_state.chunk_buffer[port_state.chunk_position++];

            // If chunk depleted, try to get next response
            if (port_state.chunk_position >= port_state.chunk_len)
            {
                ft_response_t response;
                if (queue_try_remove(&ft_response_queue, &response))
                {
                    // Verify response matches current session
                    if (response.session_id == ft_client.session_id)
                    {
                        if (response.has_count)
                        {
                            port_state.chunk_buffer[0] = response.count;
                            if (response.len > 0)
                            {
                                memcpy(&port_state.chunk_buffer[1], response.data, response.len);
                            }
                            port_state.chunk_len = response.len + 1;
                            port_state.chunk_position = 0;
                            
                            // Update file offset for stateless protocol
                            port_state.file_offset += response.len;
                        }
                        else
                        {
                            port_state.chunk_len = 0;
                            port_state.chunk_position = 0;
                        }
                        port_state.status = response.status;
                    }
                }
            }
            return byte;
        }
        return 0x00;
    }

    return 0x00;
}

// Extended output handler that handles both command and data ports
size_t files_output_ex(int port, uint8_t data, char* buffer, size_t buffer_length)
{
    if (port == 60)
    {
        return files_output(port, data, buffer, buffer_length);
    }
    else if (port == 61)
    {
        files_output_data(data);
        return 0;
    }
    return 0;
}

// ============================================================================
// Core 1: TCP Client
// ============================================================================

void ft_client_poll(void)
{
    // Check for timeout on operations
    if (ft_client.request_in_progress)
    {
        uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - ft_client.operation_start_time;
        uint32_t timeout = (ft_client.state == FT_STATE_CONNECTING) ? FT_CONNECT_TIMEOUT_MS : FT_OPERATION_TIMEOUT_MS;

        if (elapsed > timeout)
        {
            printf("[FT] Operation timeout\n");
            ft_attempt_reconnect();
            return;
        }
    }

    // State machine
    switch (ft_client.state)
    {
        case FT_STATE_DISCONNECTED:
        {
            // Check for any request - auto-connect
            ft_request_t request;
            if (queue_try_peek(&ft_request_queue, &request))
            {
                printf("[FT] Request pending, connecting...\n");
                ft_start_connect();
            }
            break;
        }

        case FT_STATE_CONNECTING:
            // Waiting for TCP connection callback
            break;

        case FT_STATE_CONNECTED:
        case FT_STATE_READY:
        {
            // Ready for operations
            if (!ft_client.request_in_progress)
            {
                ft_request_t request;
                if (queue_try_remove(&ft_request_queue, &request))
                {
                    ft_client.current_request = request;
                    ft_client.request_in_progress = true;
                    // Note: Don't reset recv_len to 0 here to support coalesced responses
                    ft_client.operation_start_time = to_ms_since_boot(get_absolute_time());

                    err_t err = ERR_OK;
                    uint8_t send_buf[260];
                    size_t send_len = 0;

                    switch (request.type)
                    {
                        case FT_REQ_GET_CHUNK:
                        {
                            // Stateless protocol: send command + offset + filename
                            send_buf[0] = FT_PROTO_GET_CHUNK;
                            
                            // Send offset (4 bytes, little-endian)
                            send_buf[1] = request.offset & 0xFF;
                            send_buf[2] = (request.offset >> 8) & 0xFF;
                            send_buf[3] = (request.offset >> 16) & 0xFF;
                            send_buf[4] = (request.offset >> 24) & 0xFF;
                            
                            // Send filename with null terminator
                            size_t name_len = strlen((char*)request.data);
                            memcpy(&send_buf[5], request.data, name_len + 1);
                            send_len = 5 + name_len + 1;
                            
                            ft_client.expected_len = 2; // status + count (payload length determined by count)
                            break;
                        }

                        case FT_REQ_CLOSE:
                        {
                            // Send command + filename for cache eviction
                            send_buf[0] = FT_PROTO_CLOSE;
                            size_t name_len = strlen((char*)request.data);
                            memcpy(&send_buf[1], request.data, name_len + 1);
                            send_len = 1 + name_len + 1;
                            ft_client.expected_len = 1; // status only
                            printf("[FT] Closing file\n");
                            break;
                        }

                        default:
                            ft_client.request_in_progress = false;
                            break;
                    }

                    if (send_len > 0 && ft_client.pcb != NULL)
                    {
                        err = tcp_write(ft_client.pcb, send_buf, send_len, TCP_WRITE_FLAG_COPY);
                        if (err == ERR_OK)
                        {
                            tcp_output(ft_client.pcb);
                            ft_client.state = FT_STATE_WAITING_RESPONSE;

                            // Check if response is already in the buffer (coalesced)
                            if (ft_client.recv_len >= ft_client.expected_len)
                            {
                                ft_handle_response();
                            }
                        }
                        else
                        {
                            printf("[FT] tcp_write error: %d\n", err);
                            ft_attempt_reconnect();
                        }
                    }
                }
            }
            break;
        }

        case FT_STATE_WAITING_RESPONSE:
            // Waiting for server response (handled in recv callback)
            break;

        case FT_STATE_RECONNECTING:
        {
            uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - ft_client.reconnect_start_time;
            if (elapsed >= FT_RECONNECT_DELAY_MS)
            {
                printf("[FT] Reconnect attempt %d/%d\n", ft_client.retry_count, FT_MAX_RETRIES);
                ft_start_connect();
            }
            break;
        }

        case FT_STATE_ERROR:
            // Fatal error - no recovery
            break;
    }
}

static void ft_start_connect(void)
{
    const char* server_ip = config_get_rfs_ip();

    if (server_ip[0] == '\0')
    {
        ft_set_error("Server IP not configured");
        return;
    }

    printf("[FT] Connecting to %s:%d\n", server_ip, FT_SERVER_PORT);

    ip_addr_t server_addr;
    if (!ip4addr_aton(server_ip, &server_addr))
    {
        ft_set_error("Invalid server IP");
        return;
    }

    ft_client.pcb = tcp_new();
    if (ft_client.pcb == NULL)
    {
        ft_set_error("Failed to create TCP PCB");
        return;
    }

    tcp_arg(ft_client.pcb, NULL);
    tcp_err(ft_client.pcb, ft_tcp_err_cb);
    tcp_recv(ft_client.pcb, ft_tcp_recv_cb);
    tcp_sent(ft_client.pcb, ft_tcp_sent_cb);

    ft_client.state = FT_STATE_CONNECTING;
    ft_client.request_in_progress = true;
    ft_client.operation_start_time = to_ms_since_boot(get_absolute_time());

    tcp_nagle_disable(ft_client.pcb);

    err_t err = tcp_connect(ft_client.pcb, &server_addr, FT_SERVER_PORT, ft_tcp_connected_cb);
    if (err != ERR_OK)
    {
        ft_set_error("TCP connect failed");
    }
}

static err_t ft_tcp_connected_cb(void* arg, struct tcp_pcb* tpcb, err_t err)
{
    (void)arg;
    (void)tpcb;

    if (err != ERR_OK)
    {
        ft_set_error("Connection failed");
        return err;
    }

    printf("[FT] Connected to server\n");
    ft_client.state = FT_STATE_READY;
    ft_client.request_in_progress = false;
    ft_client.retry_count = 0;

    return ERR_OK;
}

static void ft_tcp_err_cb(void* arg, err_t err)
{
    (void)arg;
    printf("[FT] TCP error: %d\n", err);

    ft_client.pcb = NULL;

    if (ft_client.request_in_progress)
    {
        ft_attempt_reconnect();
    }
    else
    {
        ft_client.state = FT_STATE_DISCONNECTED;
    }
}

static err_t ft_tcp_sent_cb(void* arg, struct tcp_pcb* tpcb, u16_t len)
{
    (void)arg;
    (void)tpcb;
    (void)len;
    return ERR_OK;
}

static err_t ft_tcp_recv_cb(void* arg, struct tcp_pcb* tpcb, struct pbuf* p, err_t err)
{
    (void)arg;

    if (err != ERR_OK)
    {
        if (p != NULL)
            pbuf_free(p);
        ft_set_error("Receive error");
        return err;
    }

    if (p == NULL)
    {
        ft_set_error("Server closed connection");
        return ERR_OK;
    }

    // Copy data to receive buffer
    size_t copy_len = p->tot_len;
    if (ft_client.recv_len + copy_len > FT_RECV_BUF_SIZE)
    {
        // Buffer overflow - this indicates protocol violation or attack
        printf("[FT] ERROR: Receive buffer overflow (recv_len=%zu, incoming=%zu)\n",
               ft_client.recv_len, p->tot_len);
        pbuf_free(p);
        ft_set_error("Buffer overflow");
        return ERR_ABRT;
    }

    pbuf_copy_partial(p, &ft_client.recv_buf[ft_client.recv_len], copy_len, 0);
    ft_client.recv_len += copy_len;

    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);

    // For GET_CHUNK, determine full response length once status+count are available
    if (ft_client.current_request.type == FT_REQ_GET_CHUNK && ft_client.expected_len == 2 && ft_client.recv_len >= 2)
    {
        uint8_t server_status = ft_client.recv_buf[0];
        uint8_t count_byte = ft_client.recv_buf[1];
        size_t payload_len = 0;

        if (server_status == FT_PROTO_RESP_OK || server_status == FT_PROTO_RESP_EOF)
        {
            payload_len = (count_byte == 0) ? FT_CHUNK_SIZE : count_byte;
        }
        else
        {
            payload_len = 0;
        }

        if (payload_len > FT_CHUNK_SIZE)
        {
            payload_len = FT_CHUNK_SIZE;
        }

        ft_client.expected_len = 2 + payload_len;
    }

    // Check if we have complete response
    if (ft_client.recv_len >= ft_client.expected_len)
    {
        ft_handle_response();
    }

    return ERR_OK;
}

static void ft_handle_response(void)
{
    ft_response_t response;
    memset(&response, 0, sizeof(response));

    // Tag response with current session ID
    response.session_id = ft_client.session_id;

    uint8_t server_status = ft_client.recv_buf[0];

    if (ft_client.current_request.type == FT_REQ_GET_CHUNK)
    {
        switch (server_status)
        {
            case FT_PROTO_RESP_OK:
                response.status = FT_STATUS_DATAREADY;
                break;
            case FT_PROTO_RESP_EOF:
                response.status = FT_STATUS_EOF;
                break;
            default:
                response.status = FT_STATUS_ERROR;
                break;
        }
    }
    else
    {
        response.status = (server_status == FT_PROTO_RESP_ERROR) ? FT_STATUS_ERROR : FT_STATUS_IDLE;
    }

    // Copy chunk data if this was a GET_CHUNK response
    if (ft_client.current_request.type == FT_REQ_GET_CHUNK && ft_client.recv_len >= 2)
    {
        // Format: [status:1][count:1][data:count]
        uint8_t count_byte = ft_client.recv_buf[1];
        size_t valid_bytes = 0;

        bool has_payload = (server_status == FT_PROTO_RESP_OK || server_status == FT_PROTO_RESP_EOF);

        if (has_payload)
        {
            valid_bytes = (count_byte == 0) ? 256 : count_byte;
        }
        else
        {
            // For ERROR, ignore payload
            valid_bytes = 0;
        }

        response.len = valid_bytes;
        if (response.len > FT_CHUNK_SIZE)
            response.len = FT_CHUNK_SIZE;

        response.has_count = has_payload;
        response.count = has_payload ? count_byte : 0;

        if (response.len > 0 && has_payload)
        {
            memcpy(response.data, &ft_client.recv_buf[2], response.len);
        }
    }
    else
    {
        response.has_count = false;
        response.count = 0;
        response.len = 0;
    }

    queue_try_add(&ft_response_queue, &response);

    ft_client.request_in_progress = false;

    // Shift remaining data if any (handle coalescence)
    if (ft_client.recv_len > ft_client.expected_len)
    {
        size_t remaining = ft_client.recv_len - ft_client.expected_len;
        memmove(ft_client.recv_buf, &ft_client.recv_buf[ft_client.expected_len], remaining);
        ft_client.recv_len = remaining;
    }
    else
    {
        ft_client.recv_len = 0;
    }

    ft_client.state = FT_STATE_READY;
}

static void ft_set_error(const char* msg)
{
    printf("[FT] ERROR: %s\n", msg);

    ft_client.state = FT_STATE_ERROR;
    ft_client.request_in_progress = false;

    if (ft_client.pcb != NULL)
    {
        tcp_abort(ft_client.pcb);
        ft_client.pcb = NULL;
    }

    // Send error response
    ft_response_t response;
    memset(&response, 0, sizeof(response));
    response.status = FT_STATUS_ERROR;
    response.session_id = ft_client.session_id;
    queue_try_add(&ft_response_queue, &response);
}

static void ft_attempt_reconnect(void)
{
    if (ft_client.pcb != NULL)
    {
        tcp_abort(ft_client.pcb);
        ft_client.pcb = NULL;
    }

    // Clear receive buffer to prevent stale data corruption
    ft_client.recv_len = 0;
    memset(ft_client.recv_buf, 0, sizeof(ft_client.recv_buf));

    ft_client.retry_count++;

    if (ft_client.retry_count > FT_MAX_RETRIES)
    {
        ft_set_error("Max retries exceeded");
        return;
    }

    printf("[FT] Connection lost, will retry (%d/%d)...\n", ft_client.retry_count, FT_MAX_RETRIES);

    ft_client.request_in_progress = false;
    ft_client.state = FT_STATE_RECONNECTING;
    ft_client.reconnect_start_time = to_ms_since_boot(get_absolute_time());
}

#else // !CYW43_WL_GPIO_LED_PIN - Stub implementations for non-WiFi boards

#include <stddef.h>
#include <stdint.h>

void files_io_init(void) {}

size_t files_output(int port, uint8_t data, char* buffer, size_t buffer_length)
{
    (void)port;
    (void)data;
    (void)buffer;
    (void)buffer_length;
    return 0;
}

uint8_t files_input(uint8_t port)
{
    (void)port;
    return 0xFF; // Return error status
}

void ft_client_poll(void) {}

#endif // CYW43_WL_GPIO_LED_PIN
