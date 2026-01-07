#include "http_get.h"

#include "pico/stdlib.h" // Must be included before WiFi check to get board definitions

// HTTP GET is only available on WiFi-enabled boards
#if defined(CYW43_WL_GPIO_LED_PIN)

#include <stdio.h>
#include <string.h>

#include "pico/mutex.h"      // For mutex_t
#include "pico/util/queue.h" // For queue_t

#include "lwip/altcp.h"
#include "lwip/apps/http_client.h"
#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/pbuf.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"

// Queue sizes
#define OUTBOUND_QUEUE_SIZE 4
#define INBOUND_QUEUE_SIZE 8 // Increased to 8 to cover small file bursts (2KB)

// Queues for inter-core communication
static queue_t outbound_queue; // Core 0 -> Core 1
static queue_t inbound_queue;  // Core 1 -> Core 0

// State variables
static http_transfer_state_t transfer_state;

// Mutex for protecting transfer state
static mutex_t transfer_mutex;

// === CORE 1: HTTP Client ===

// lwIP HTTP client callback: receive data
static err_t http_recv_callback(void* arg, struct altcp_pcb* conn, struct pbuf* p, err_t err)
{
    http_transfer_state_t* state = (http_transfer_state_t*)arg;

    // Store connection handle for async TCP ACKs in poll loop
    state->conn = conn;

    if (err != ERR_OK || p == NULL)
    {
        if (p != NULL)
        {
            pbuf_free(p);
        }
        return err;
    }

    // If we already have a pending pbuf (in-flight data), chain it to avoid
    // aborting the connection. TCP window should close naturally.
    if (state->pending_pbuf != NULL)
    {
        // Chain the new pbuf to the end of existing pending pbuf
        pbuf_cat(state->pending_pbuf, p);

        // Return ERR_OK to accept the data (but we don't ACK it yet)
        return ERR_OK;
    }

    // Process received data
    // We try to push as much as possible to the queue
    struct pbuf* current = p;
    size_t offset = 0;

    // Iterate through pbuf chain
    while (current != NULL)
    {
        // Process current pbuf
        while (offset < current->len)
        {
            // Determine how much can fit in current chunk
            size_t bytes_available = current->len - offset;
            size_t chunk_space = HTTP_CHUNK_SIZE - state->current_chunk.len;
            size_t copy_size = (bytes_available > chunk_space) ? chunk_space : bytes_available;

            // Copy to chunk
            uint8_t* payload = (uint8_t*)current->payload;
            memcpy(&state->current_chunk.data[state->current_chunk.len], &payload[offset], copy_size);

            state->current_chunk.len += copy_size;
            offset += copy_size;

            // If chunk is full, send to queue
            if (state->current_chunk.len >= HTTP_CHUNK_SIZE)
            {
                state->current_chunk.status = HTTP_WG_DATAREADY;

                // Try to queue non-blocking
                if (queue_try_add(&inbound_queue, &state->current_chunk))
                {
                    // Successfully queued - reset chunk
                    memset(&state->current_chunk, 0, sizeof(state->current_chunk));
                }
                else
                {
                    // Queue full - pause and store pbuf for later processing
                    state->pending_pbuf = p;

                    // Calculate global offset into pbuf chain
                    size_t total_processed_so_far = 0;
                    struct pbuf* temp = p;
                    while (temp != current)
                    {
                        total_processed_so_far += temp->len;
                        temp = temp->next;
                    }
                    total_processed_so_far += offset;
                    state->pending_pbuf_offset = total_processed_so_far;

                    // Don't ACK unprocessed data - closes TCP window
                    return ERR_OK;
                }
            }
        }

        // Finished this pbuf in chain, move to next
        current = current->next;
        offset = 0;
    }

    // Consumed entire pbuf chain - free and ACK
    // (Wait, we can't ACK bytes inside chunks that are still in queue?
    //  Actually we can, because we've *accepted* them into our system.
    //  The queue acts as the receive buffer extension.)
    size_t pbuf_total = p->tot_len; // Save before free
    pbuf_free(p);
    state->total_bytes_received += pbuf_total;
    altcp_recved(conn, pbuf_total);

    return ERR_OK;
}

// lwIP HTTP client callback: headers received
static err_t http_headers_done_callback(httpc_state_t* connection, void* arg, struct pbuf* hdr, u16_t hdr_len,
                                        u32_t content_len)
{
    (void)connection;
    (void)hdr;
    (void)hdr_len;
    // Headers received - content length available
    (void)content_len;
    return ERR_OK;
}

// lwIP HTTP client callback: transfer complete
static void http_result_callback(void* arg, httpc_result_t httpc_result, u32_t rx_content_len, u32_t srv_res, err_t err)
{
    http_transfer_state_t* state = (http_transfer_state_t*)arg;

    // Prepare final status message (calculated early for both paths)
    memset(&state->final_status, 0, sizeof(state->final_status));
    if (httpc_result == HTTPC_RESULT_OK && srv_res >= 200 && srv_res < 300)
    {
        state->final_status.status = HTTP_WG_EOF;
    }
    else
    {
        state->final_status.status = HTTP_WG_FAILED;
    }
    state->final_status.len = 0;

    // If we have pending data frozen in a pbuf (paused), we MUST NOT queue new things yet.
    // Core 0 must receive the pending pbuf data BEFORE valid EOF.
    if (state->pending_pbuf != NULL)
    {
        // Defer sending status until http_get_poll drains the pbuf
        state->pending_final_status = true;

        // Note: We do NOT touch current_chunk or set pending_final_chunk here.
        // The poll loop is actively using current_chunk to drain the pbuf.
        // It will handle flushing the remainder when the pbuf is empty.

        state->transfer_active = false;
        state->transfer_complete = true;
        return;
    }

    // Normal path (no paused pbuf) - Send final chunk if there's data
    if (state->current_chunk.len > 0)
    {
        if (state->final_status.status == HTTP_WG_EOF)
        {
            state->current_chunk.status = HTTP_WG_DATAREADY;
        }
        else
        {
            state->current_chunk.status = HTTP_WG_FAILED;
        }

        // Try to queue, but don't block
        if (!queue_try_add(&inbound_queue, &state->current_chunk))
        {
            // Queue full - save for retry in http_get_poll()
            state->final_chunk = state->current_chunk;
            state->pending_final_chunk = true;
        }
    }

    // Try to queue final status, but ONLY if final data chunk succeeded
    // Otherwise defer - EOF must come AFTER all data
    if (!state->pending_final_chunk)
    {
        if (!queue_try_add(&inbound_queue, &state->final_status))
        {
            state->pending_final_status = true;
        }
    }
    else
    {
        // Data chunk pending - must wait to send EOF after data
        state->pending_final_status = true;
    }

    state->transfer_active = false;
    state->transfer_complete = true;
    state->conn = NULL; // Clear connection handle to prevent use-after-free
}

// Parse URL to extract hostname/IP, port, and path
// Supports: http://hostname:port/path or just hostname:port/path
// Returns 0 on success, -1 on error
static int parse_url(const char* url, char* hostname, size_t hostname_len, u16_t* port, char* path, size_t path_len)
{
    const char* start = url;
    const char* end;

    // Skip "http://" prefix if present
    if (strncmp(url, "http://", 7) == 0)
    {
        start = url + 7;
    }
    else if (strncmp(url, "HTTP://", 7) == 0)
    {
        start = url + 7;
    }

    // Find end of hostname (either ':', '/', or end of string)
    end = start;
    while (*end != '\0' && *end != ':' && *end != '/')
    {
        end++;
    }

    // Extract hostname
    size_t host_len = end - start;
    if (host_len >= hostname_len)
    {
        return -1; // Hostname too long
    }
    memcpy(hostname, start, host_len);
    hostname[host_len] = '\0';

    // Default port
    *port = 80;

    // Check for port specification
    if (*end == ':')
    {
        // Parse port number
        end++; // Skip ':'
        const char* port_start = end;
        unsigned long parsed_port = 0;

        while (*end >= '0' && *end <= '9')
        {
            parsed_port = parsed_port * 10 + (*end - '0');
            end++;
        }

        if (end > port_start && parsed_port > 0 && parsed_port <= 65535)
        {
            *port = (u16_t)parsed_port;
        }
        else
        {
            return -1; // Invalid port
        }
    }

    // Extract path (everything after hostname:port)
    if (*end == '/')
    {
        size_t path_length = strlen(end);
        if (path_length >= path_len)
        {
            return -1; // Path too long
        }
        strcpy(path, end);
    }
    else
    {
        // No path specified, use root
        strcpy(path, "/");
    }

    return 0;
}

void http_get_init(void)
{
    // Initialize queues
    queue_init(&outbound_queue, sizeof(http_request_t), OUTBOUND_QUEUE_SIZE);
    queue_init(&inbound_queue, sizeof(http_response_t), INBOUND_QUEUE_SIZE);

    // Initialize state
    memset(&transfer_state, 0, sizeof(transfer_state));
}

void http_get_poll(void)
{
    // PRIORITY 1: SERVICE PENDING BUFFERS (Resume Flow Control)
    // CRITICAL: Must drain pbufs BEFORE sending final messages!
    if (transfer_state.pending_pbuf != NULL)
    {
        // We have data that was paused because queue was full. Try to drain it now.
        struct pbuf* p = transfer_state.pending_pbuf;
        struct pbuf* current = p;
        size_t current_offset = 0;
        size_t target_offset = transfer_state.pending_pbuf_offset;

        // Fast-forward to where we left off
        while (current != NULL)
        {
            if (current_offset + current->len > target_offset)
            {
                break;
            }
            current_offset += current->len;
            current = current->next;
        }

        // Local offset within the 'current' pbuf
        size_t local_offset = target_offset - current_offset;
        bool stuck = false;

        while (current != NULL && !stuck)
        {
            while (local_offset < current->len)
            {
                // Determine how much can fit in current chunk
                size_t bytes_available = current->len - local_offset;
                size_t chunk_space = HTTP_CHUNK_SIZE - transfer_state.current_chunk.len;
                size_t copy_size = (bytes_available > chunk_space) ? chunk_space : bytes_available;

                // Copy to chunk
                uint8_t* payload = (uint8_t*)current->payload;
                memcpy(&transfer_state.current_chunk.data[transfer_state.current_chunk.len], &payload[local_offset],
                       copy_size);

                transfer_state.current_chunk.len += copy_size;
                local_offset += copy_size;
                transfer_state.pending_pbuf_offset += copy_size; // Advance global offset

                // If chunk is full, send to queue
                if (transfer_state.current_chunk.len >= HTTP_CHUNK_SIZE)
                {
                    transfer_state.current_chunk.status = HTTP_WG_DATAREADY;

                    if (queue_try_add(&inbound_queue, &transfer_state.current_chunk))
                    {
                        // Success! ACK this chunk.
                        // This re-opens the TCP window, signaling the server to send more.
                        if (transfer_state.conn)
                        {
                            altcp_recved((struct tcp_pcb*)transfer_state.conn, HTTP_CHUNK_SIZE);
                        }

                        memset(&transfer_state.current_chunk, 0, sizeof(transfer_state.current_chunk));
                    }
                    else
                    {
                        // Still stuck. Stop draining.
                        stuck = true;
                        break;
                    }
                }
            }

            if (!stuck)
            {
                current = current->next;
                local_offset = 0;
            }
        }

        // If we finished the entire chain
        if (current == NULL && !stuck)
        {
            // ACK any remaining bytes buffered in current_chunk
            if (transfer_state.conn && transfer_state.current_chunk.len > 0)
            {
                altcp_recved((struct tcp_pcb*)transfer_state.conn, transfer_state.current_chunk.len);
            }

            pbuf_free(p);
            transfer_state.pending_pbuf = NULL;
            transfer_state.pending_pbuf_offset = 0;
        }
    }

    // PRIORITY 2: Handle Final Messages (Only if no pending pbuf)
    if (transfer_state.pending_pbuf == NULL)
    {
        // 2a. Flush any partial data remaining in current_chunk
        // This happens when http_result_callback deferred because pbuf was pending
        if (transfer_state.pending_final_status && transfer_state.current_chunk.len > 0)
        {
            // We have leftover data that needs to be sent before EOF status
            if (transfer_state.final_status.status == HTTP_WG_EOF)
            {
                transfer_state.current_chunk.status = HTTP_WG_DATAREADY;
            }
            else
            {
                transfer_state.current_chunk.status = HTTP_WG_FAILED;
            }

            if (queue_try_add(&inbound_queue, &transfer_state.current_chunk))
            {
                // Success, clear it
                memset(&transfer_state.current_chunk, 0, sizeof(transfer_state.current_chunk));
            }
            else
            {
                // Queue full, will retry next poll
                return;
            }
        }

        // 2b. Retry pending final chunk (from normal result callback path)
        if (transfer_state.pending_final_chunk)
        {
            if (queue_try_add(&inbound_queue, &transfer_state.final_chunk))
            {
                transfer_state.pending_final_chunk = false;
            }
        }

        // 2c. Send Final Status (EOF/Fail)
        // Only if preceding data chunks are clear
        if (!transfer_state.pending_final_chunk && transfer_state.current_chunk.len == 0)
        {
            if (transfer_state.pending_final_status)
            {
                if (queue_try_add(&inbound_queue, &transfer_state.final_status))
                {
                    transfer_state.pending_final_status = false;
                }
            }
        }
    }

    // Check for new HTTP requests from Core 0
    http_request_t request;

    if (queue_try_remove(&outbound_queue, &request))
    {
        if (request.abort)
        {
            // Clean up any pending state
            if (transfer_state.pending_pbuf)
            {
                pbuf_free(transfer_state.pending_pbuf);
            }
            memset(&transfer_state, 0, sizeof(transfer_state));
            return;
        }

        // Parse URL to extract hostname, port, and path
        char hostname[128];
        char path[128];
        u16_t port;

        if (parse_url(request.url, hostname, sizeof(hostname), &port, path, sizeof(path)) != 0)
        {
            // Send failure status
            http_response_t response;
            memset(&response, 0, sizeof(response));
            response.status = HTTP_WG_FAILED;
            response.len = 0;
            queue_try_add(&inbound_queue, &response);
            return;
        }

        // Build full URL for lwIP (hostname/path, port separate)
        char full_url[256];
        snprintf(full_url, sizeof(full_url), "%s%s", hostname, path);
        (void)full_url; // Used for debugging if needed

        // Reset transfer state
        memset(&transfer_state, 0, sizeof(transfer_state));
        transfer_state.transfer_active = true;

        // Configure HTTP client settings
        httpc_connection_t settings;
        memset(&settings, 0, sizeof(settings));
        settings.use_proxy = 0;
        settings.result_fn = http_result_callback;
        settings.headers_done_fn = http_headers_done_callback;

        // Start HTTP GET request with parsed port
        httpc_state_t* connection = NULL;
        err_t err =
            httpc_get_file_dns(hostname, port, path, &settings, http_recv_callback, &transfer_state, &connection);

        if (err != ERR_OK)
        {
            // Send failure status
            http_response_t response;
            memset(&response, 0, sizeof(response));
            response.status = HTTP_WG_FAILED;
            response.len = 0;
            queue_try_add(&inbound_queue, &response);

            transfer_state.transfer_active = false;
        }
    }
}

void http_get_queues(queue_t** outbound, queue_t** inbound)
{
    *outbound = &outbound_queue;
    *inbound = &inbound_queue;
}

#else // !CYW43_WL_GPIO_LED_PIN - Stub implementations for non-WiFi boards

void http_get_init(void)
{
    // No-op on non-WiFi boards
}

void http_get_poll(void)
{
    // No-op on non-WiFi boards
}

void http_get_queues(queue_t** outbound, queue_t** inbound)
{
    // No-op on non-WiFi boards
    *outbound = NULL;
    *inbound = NULL;
}

#endif // CYW43_WL_GPIO_LED_PIN
