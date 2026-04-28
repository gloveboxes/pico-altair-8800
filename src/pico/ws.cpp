/**
 * @file ws.cpp
 * @brief WebSocket server wrapper for Pico using pico-ws-server
 *
 * Single-client model: Only one WebSocket client at a time. New connections
 * are rejected while a client is active.
 * 
 * Architecture for pico_cyw43_arch_lwip_threadsafe_background:
 * - The pico-ws-server library handles lwIP threading internally
 * - Callbacks run in lwIP context, so keep them minimal (set flags)
 * - Main loop calls popMessages() and processes flags
 */

#include "ws.h"

#include "pico/time.h"
#include "pico_ws_server/web_socket_server.h"

#include <cstdio>
#include <cstring>
#include <memory>

namespace
{
static constexpr uint16_t WS_SERVER_PORT = 8088;
// The pico-ws-server `max_connections` limit counts *all* TCP connections (including
// non-upgraded HTTP connections used to serve the UI). Keep this higher to avoid
// RSTs during the WebSocket handshake when a browser holds an HTTP keep-alive connection open.
static constexpr uint32_t WS_SERVER_MAX_CONNECTIONS = 8;
static constexpr size_t WS_FRAME_PAYLOAD = 256;
static constexpr uint32_t WS_PING_INTERVAL_MS = 10000; // 10s
static constexpr uint8_t WS_MAX_PING_FAILURES = 3;     // 30s total timeout

// Receive buffer for deferred message processing
static constexpr size_t WS_RECV_BUFFER_SIZE = 512;

struct ws_context_t
{
    ws_callbacks_t callbacks;
};

static ws_context_t g_ws_context = {};
static bool g_ws_initialized = false;
static bool g_ws_running = false;
static std::unique_ptr<WebSocketServer> g_ws_server;

// Single WebSocket client connection (-1 = no client)
static volatile int32_t g_client_conn_id = -1;
static absolute_time_t g_next_ping_deadline;
static uint8_t g_ping_failures = 0;

// Flags set by callbacks, processed by main loop
static volatile bool g_ping_due = false;
static volatile bool g_client_connected = false;
static volatile bool g_client_disconnected = false;
static volatile bool g_close_requested = false;
static volatile bool g_pong_received = false;

// Pending connection to reject (set by callback, processed by main loop)
static volatile int32_t g_pending_reject_conn_id = -1;

// Received message buffer (callback copies data, main loop processes)
static volatile bool g_message_pending = false;
static uint8_t g_recv_buffer[WS_RECV_BUFFER_SIZE];
static volatile size_t g_recv_len = 0;

// ============================================================================
// Timer check (main loop context)
// ============================================================================

static void check_ping_timer(void)
{
    if (g_client_conn_id < 0)
    {
        return;
    }

    absolute_time_t now = get_absolute_time();

    // Check if it's time to send a ping - just set the flag
    if (absolute_time_diff_us(now, g_next_ping_deadline) <= 0)
    {
        g_ping_due = true;
    }
}

// ============================================================================
// Callbacks (lwIP context - minimal work, just set flags)
// ============================================================================

void handle_connect(WebSocketServer& server, uint32_t conn_id)
{
    (void)server;
    
    int32_t old_id = g_client_conn_id;

    // If there's already an active client, mark new connection for rejection
    if (old_id >= 0 && static_cast<uint32_t>(old_id) != conn_id)
    {
        g_pending_reject_conn_id = static_cast<int32_t>(conn_id);
#ifdef ALTAIR_DEBUG
        printf("[CB] WebSocket rejecting new client %u - existing client %d still active\n", conn_id, old_id);
#endif
        return;
    }

    // Accept new client - set state and flag
    g_client_conn_id = static_cast<int32_t>(conn_id);
    g_ping_failures = 0;
    g_next_ping_deadline = make_timeout_time_ms(WS_PING_INTERVAL_MS);
    g_client_connected = true;

#ifdef ALTAIR_DEBUG
    printf("[CB] WebSocket client connected (id=%u)\n", conn_id);
#endif
}

void handle_close(WebSocketServer& server, uint32_t conn_id)
{
    (void)server;

    // Only set flag if this was our active client
    if (g_client_conn_id >= 0 && static_cast<uint32_t>(g_client_conn_id) == conn_id)
    {
        g_client_conn_id = -1;
        g_ping_failures = 0;
        g_client_disconnected = true;
#ifdef ALTAIR_DEBUG
        printf("[CB] WebSocket client disconnected (id=%u)\n", conn_id);
#endif
    }
#ifdef ALTAIR_DEBUG
    else
    {
        printf("[CB] WebSocket close for non-active conn_id=%u (active=%d)\n", conn_id, g_client_conn_id);
    }
#endif
}

void handle_message(WebSocketServer& server, uint32_t conn_id, const void* data, size_t len)
{
    (void)server;
    
    // Ignore messages from non-active clients
    if (g_client_conn_id < 0 || static_cast<uint32_t>(g_client_conn_id) != conn_id)
    {
        return;
    }

    // Copy message to buffer for main loop processing
    if (!g_message_pending && len <= WS_RECV_BUFFER_SIZE)
    {
        std::memcpy(g_recv_buffer, data, len);
        g_recv_len = len;
        g_message_pending = true;
#ifdef ALTAIR_DEBUG
        printf("[CB] WebSocket message queued (%zu bytes)\n", len);
#endif
    }
#ifdef ALTAIR_DEBUG
    else
    {
        printf("[CB] WebSocket message dropped (pending=%d, len=%zu)\n", g_message_pending, len);
    }
#endif
}

void handle_pong(WebSocketServer& server, uint32_t conn_id, const void* data, size_t len)
{
    (void)server;
    (void)data;
    (void)len;

    // Only process pong from active client
    if (g_client_conn_id < 0 || static_cast<uint32_t>(g_client_conn_id) != conn_id)
    {
        return;
    }

    g_pong_received = true;
#ifdef ALTAIR_DEBUG
    printf("[CB] WebSocket received PONG from %u\n", conn_id);
#endif
}

// ============================================================================
// Main loop processing
// ============================================================================

static void process_pending_events(void)
{
    // Process pending connection rejection
    if (g_pending_reject_conn_id >= 0)
    {
        uint32_t reject_id = static_cast<uint32_t>(g_pending_reject_conn_id);
        g_pending_reject_conn_id = -1;
        g_ws_server->close(reject_id);
#ifdef ALTAIR_DEBUG
        printf("WebSocket rejected connection %u\n", reject_id);
#endif
    }

    // Process client connected notification
    if (g_client_connected)
    {
        g_client_connected = false;
        if (g_ws_context.callbacks.on_client_connected)
        {
            g_ws_context.callbacks.on_client_connected(g_ws_context.callbacks.user_data);
        }
    }

    // Process client disconnected notification
    if (g_client_disconnected)
    {
        g_client_disconnected = false;
        if (g_ws_context.callbacks.on_client_disconnected)
        {
            g_ws_context.callbacks.on_client_disconnected(g_ws_context.callbacks.user_data);
        }
    }

    // Process pong received
    if (g_pong_received)
    {
        g_pong_received = false;
        g_ping_failures = 0;
        g_next_ping_deadline = make_timeout_time_ms(WS_PING_INTERVAL_MS);
    }

    // Process received message
    if (g_message_pending && g_ws_context.callbacks.on_receive)
    {
        size_t len = g_recv_len;
        g_message_pending = false;
        
        bool keep_open = g_ws_context.callbacks.on_receive(g_recv_buffer, len, g_ws_context.callbacks.user_data);
        
        if (!keep_open && g_client_conn_id >= 0)
        {
            g_close_requested = true;
        }
    }

    // Process close request
    if (g_close_requested && g_client_conn_id >= 0)
    {
        g_close_requested = false;
        int32_t conn_id = g_client_conn_id;
        g_client_conn_id = -1;
        g_ws_server->close(static_cast<uint32_t>(conn_id));
    }
}

static void send_ping(void)
{
    if (!g_ws_running || !g_ws_server || g_client_conn_id < 0 || !g_ping_due)
    {
        return;
    }

    g_ping_due = false;

    bool ping_sent = g_ws_server->sendPing(static_cast<uint32_t>(g_client_conn_id), nullptr, 0);
    
    if (ping_sent)
    {
#ifdef ALTAIR_DEBUG
        printf("WebSocket sent PING to %d\n", g_client_conn_id);
#endif
    }
    else
    {
        ++g_ping_failures;
#ifdef ALTAIR_DEBUG
        printf("WebSocket PING send failed (failures=%u)\n", g_ping_failures);
#endif
    }

    // Close if too many ping failures (connection is dead)
    if (g_ping_failures >= WS_MAX_PING_FAILURES)
    {
#ifdef ALTAIR_DEBUG
        printf("WebSocket closing connection %d after %u ping failures\n", g_client_conn_id, g_ping_failures);
#endif
        int32_t old_id = g_client_conn_id;
        g_client_conn_id = -1;
        g_ping_failures = 0;
        g_ws_server->close(static_cast<uint32_t>(old_id));
        return;
    }

    g_next_ping_deadline = make_timeout_time_ms(WS_PING_INTERVAL_MS);
}

} // namespace

extern "C"
{

    void ws_init(const ws_callbacks_t* callbacks)
    {
        if (!callbacks)
        {
            std::memset(&g_ws_context, 0, sizeof(g_ws_context));
            g_ws_initialized = false;
            return;
        }

        g_ws_context.callbacks = *callbacks;
        g_ws_initialized = true;
    }

    bool ws_start(void)
    {
        if (!g_ws_initialized)
        {
#ifdef ALTAIR_DEBUG
            printf("WebSocket server not initialized\n");
#endif
            return false;
        }

        if (g_ws_running)
        {
            return true;
        }

        if (!g_ws_server)
        {
            g_ws_server = std::make_unique<WebSocketServer>(WS_SERVER_MAX_CONNECTIONS);
            g_ws_server->setCallbackExtra(&g_ws_context);
            g_ws_server->setConnectCallback(handle_connect);
            g_ws_server->setCloseCallback(handle_close);
            g_ws_server->setMessageCallback(handle_message);
            g_ws_server->setPongCallback(handle_pong);
            g_ws_server->setTcpNoDelay(true); // Disable Nagle's algorithm for low latency
        }

        if (!g_ws_server->startListening(WS_SERVER_PORT))
        {
#ifdef ALTAIR_DEBUG
            printf("Failed to start WebSocket server on port %u\n", WS_SERVER_PORT);
#endif
            g_ws_server.reset();
            g_ws_running = false;
            return false;
        }

        g_ws_running = true;
#ifdef ALTAIR_DEBUG
        printf("WebSocket server listening on port %u\n", WS_SERVER_PORT);
#endif
        return true;
    }

    bool ws_is_running(void)
    {
        return g_ws_running && g_ws_server != nullptr;
    }

    bool ws_has_active_clients(void)
    {
        return g_client_conn_id >= 0;
    }

    void ws_poll_incoming(void)
    {
        if (!g_ws_running || !g_ws_server)
        {
            return;
        }

        // Process incoming messages (triggers callbacks)
        g_ws_server->popMessages();

        // Process any flags set by callbacks
        process_pending_events();

        // Heartbeat: check if ping is due, then send if flagged
        if (g_client_conn_id >= 0)
        {
            check_ping_timer();
            send_ping();
        }
    }

    void ws_poll_outgoing(void)
    {
        if (!g_ws_running || !g_ws_server)
        {
            return;
        }

        if (g_client_conn_id < 0 || !g_ws_context.callbacks.on_output)
        {
            return;
        }

        uint8_t payload[WS_FRAME_PAYLOAD];

        size_t payload_len =
            g_ws_context.callbacks.on_output(payload, sizeof(payload), g_ws_context.callbacks.user_data);
        if (payload_len == 0)
        {
            return;
        }

#ifdef ALTAIR_DEBUG
        printf("WebSocket sending %zu bytes to %d\n", payload_len, g_client_conn_id);
#endif

        if (!g_ws_server->sendMessage(static_cast<uint32_t>(g_client_conn_id), payload, payload_len))
        {
#ifdef ALTAIR_DEBUG
            // TCP send buffer is full - drop this frame (expected under load)
            printf("WebSocket send failed, dropping %zu bytes\n", payload_len);
#endif
        }
    }

    uint32_t ws_get_connection_state(void)
    {
        // Return: 1 if connected, 0 if not (simplified from multi-client version)
        return (g_client_conn_id >= 0) ? 1 : 0;
    }
}
