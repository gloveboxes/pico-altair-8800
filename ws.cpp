/**
 * @file ws.cpp
 * @brief WebSocket server wrapper for Pico using pico-ws-server
 *
 * Single-client model: Only one WebSocket client at a time. New connections
 * kick the existing client, which handles browser refresh gracefully.
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

static void send_ping_if_due(void)
{
    if (!g_ws_running || !g_ws_server || g_client_conn_id < 0)
    {
        return;
    }

    absolute_time_t now = get_absolute_time();

    // Check if it's time to send a ping
    if (absolute_time_diff_us(now, g_next_ping_deadline) > 0)
    {
        return; // Not time yet
    }

    // Send ping
    bool ping_sent = g_ws_server->sendPing(static_cast<uint32_t>(g_client_conn_id), nullptr, 0);
    if (ping_sent)
    {
        g_ping_failures = 0;
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
        g_client_conn_id = -1; // Clear before triggering close
        g_ws_server->close(static_cast<uint32_t>(old_id));
        g_ping_failures = 0;
        return;
    }

    g_next_ping_deadline = delayed_by_ms(now, WS_PING_INTERVAL_MS);
}

void handle_connect(WebSocketServer& server, uint32_t conn_id)
{
    int32_t old_id = g_client_conn_id;

    // Kick existing client if any (new connection takes over)
    if (old_id >= 0 && static_cast<uint32_t>(old_id) != conn_id)
    {
#ifdef ALTAIR_DEBUG
        printf("WebSocket kicking existing client %d for new client %u\n", old_id, conn_id);
#endif
        g_client_conn_id = -1; // Clear before triggering close
        server.close(static_cast<uint32_t>(old_id));
    }

    // Accept new client
    g_client_conn_id = static_cast<int32_t>(conn_id);
    g_ping_failures = 0;
    g_next_ping_deadline = make_timeout_time_ms(WS_PING_INTERVAL_MS);

#ifdef ALTAIR_DEBUG
    printf("WebSocket client connected (id=%u)\n", conn_id);
#endif

    ws_context_t* ctx = static_cast<ws_context_t*>(server.getCallbackExtra());
    if (ctx && ctx->callbacks.on_client_connected)
    {
        ctx->callbacks.on_client_connected(ctx->callbacks.user_data);
    }
}

void handle_close(WebSocketServer& server, uint32_t conn_id)
{
    (void)server;

    // Only notify if this was our active client
    if (g_client_conn_id >= 0 && static_cast<uint32_t>(g_client_conn_id) == conn_id)
    {
        g_client_conn_id = -1;
        g_ping_failures = 0;
#ifdef ALTAIR_DEBUG
        printf("WebSocket client disconnected (id=%u)\n", conn_id);
#endif

        ws_context_t* ctx = static_cast<ws_context_t*>(server.getCallbackExtra());
        if (ctx && ctx->callbacks.on_client_disconnected)
        {
            ctx->callbacks.on_client_disconnected(ctx->callbacks.user_data);
        }
    }
#ifdef ALTAIR_DEBUG
    else
    {
        // Close for unknown/already-kicked connection
        printf("WebSocket close for non-active conn_id=%u (active=%d)\n", conn_id, g_client_conn_id);
    }
#endif
}

void handle_message(WebSocketServer& server, uint32_t conn_id, const void* data, size_t len)
{
    // Ignore messages from non-active clients
    if (g_client_conn_id < 0 || static_cast<uint32_t>(g_client_conn_id) != conn_id)
    {
        return;
    }

    ws_context_t* ctx = static_cast<ws_context_t*>(server.getCallbackExtra());
    if (!ctx)
    {
        return;
    }

    bool keep_open = true;
    if (ctx->callbacks.on_receive)
    {
        keep_open = ctx->callbacks.on_receive(static_cast<const uint8_t*>(data), len, ctx->callbacks.user_data);
    }

    if (!keep_open)
    {
        g_client_conn_id = -1; // Clear before triggering close
        server.close(conn_id);
    }
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

    g_ping_failures = 0;
    g_next_ping_deadline = make_timeout_time_ms(WS_PING_INTERVAL_MS);
#ifdef ALTAIR_DEBUG
    printf("WebSocket received PONG from %u\n", conn_id);
#endif
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

        // Always process messages (handles close race conditions)
        g_ws_server->popMessages();

        if (g_client_conn_id < 0)
        {
            return;
        }

        // Heartbeat: check timer for pings
        send_ping_if_due();
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
