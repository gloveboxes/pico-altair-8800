#include "ws.h"

#include "pico/time.h"
#include "pico_ws_server/web_socket_server.h"

#include <cstdio>
#include <cstring>
#include <memory>

namespace
{
static constexpr uint16_t WS_SERVER_PORT = 8088;
static constexpr uint32_t WS_MAX_CLIENTS = 2;
// The pico-ws-server `max_connections` limit counts *all* TCP connections (including
// non-upgraded HTTP connections used to serve the UI). Keep this higher than
// WS_MAX_CLIENTS to avoid RSTs during the WebSocket handshake when a browser
// holds an HTTP keep-alive connection open.
static constexpr uint32_t WS_SERVER_MAX_CONNECTIONS = 8;
static constexpr size_t WS_FRAME_PAYLOAD = 256;
static constexpr uint32_t WS_PING_INTERVAL_MS = 10000; // 10s
static constexpr uint8_t WS_MAX_MISSED_PONGS = 3;      // 30s total timeout

struct ws_context_t
{
    ws_callbacks_t callbacks;
};

struct ws_connection_state_t
{
    uint32_t conn_id;
    absolute_time_t next_ping_deadline;
    uint8_t pending_pings;
    uint8_t missed_pongs;
    bool active;
    bool closing;
};

static ws_context_t g_ws_context = {};
static bool g_ws_initialized = false;
static bool g_ws_running = false;
static size_t g_ws_active_clients = 0;
static std::unique_ptr<WebSocketServer> g_ws_server;
static ws_connection_state_t g_ws_connections[WS_MAX_CLIENTS] = {};

static ws_connection_state_t* find_connection(uint32_t conn_id)
{
    for (size_t i = 0; i < WS_MAX_CLIENTS; ++i)
    {
        if (g_ws_connections[i].active && g_ws_connections[i].conn_id == conn_id)
        {
            return &g_ws_connections[i];
        }
    }
    return nullptr;
}

static ws_connection_state_t* allocate_connection(uint32_t conn_id)
{
    for (size_t i = 0; i < WS_MAX_CLIENTS; ++i)
    {
        if (!g_ws_connections[i].active)
        {
            g_ws_connections[i].conn_id = conn_id;
            g_ws_connections[i].active = true;
            g_ws_connections[i].closing = false;
            g_ws_connections[i].pending_pings = 0;
            g_ws_connections[i].missed_pongs = 0;
            g_ws_connections[i].next_ping_deadline = make_timeout_time_ms(WS_PING_INTERVAL_MS);
            return &g_ws_connections[i];
        }
    }
    return nullptr;
}

static void send_ping_if_due(void)
{
    if (!g_ws_running || !g_ws_server || g_ws_active_clients == 0)
    {
        return;
    }

    absolute_time_t now = get_absolute_time();

    // Check each active connection
    for (size_t i = 0; i < WS_MAX_CLIENTS; ++i)
    {
        ws_connection_state_t* conn = &g_ws_connections[i];
        if (!conn->active || conn->closing)
        {
            continue;
        }

        // Check if it's time to send a ping for this connection
        if (absolute_time_diff_us(now, conn->next_ping_deadline) > 0)
        {
            continue; // Not time yet
        }

        // Check for missed pongs
        if (conn->pending_pings > 0)
        {
            ++conn->missed_pongs;
        }

        // Send ping
        bool ping_sent = false;
        if (conn->missed_pongs <= WS_MAX_MISSED_PONGS)
        {
            ping_sent = g_ws_server->sendPing(conn->conn_id, nullptr, 0);
            if (ping_sent)
            {
                ++conn->pending_pings;
#ifdef ALTAIR_DEBUG
                printf("WebSocket sent PING to %u (pending=%u, missed=%u)\n", conn->conn_id, conn->pending_pings,
                       conn->missed_pongs);
#endif
            }
            else
            {
                ++conn->missed_pongs;
#ifdef ALTAIR_DEBUG
                printf("WebSocket PING send failed to %u (missed=%u)\n", conn->conn_id, conn->missed_pongs);
#endif
            }
        }

        // Close if too many missed pongs
        if (conn->missed_pongs > WS_MAX_MISSED_PONGS)
        {
#ifdef ALTAIR_DEBUG
            printf("WebSocket closing connection %u after %u missed pongs\n", conn->conn_id, conn->missed_pongs);
#endif
            g_ws_server->close(conn->conn_id);
            conn->closing = true;
            continue;
        }

        conn->next_ping_deadline = delayed_by_ms(now, WS_PING_INTERVAL_MS);
    }
}

void handle_connect(WebSocketServer& server, uint32_t conn_id)
{
    ws_connection_state_t* conn = allocate_connection(conn_id);
    if (!conn)
    {
#ifdef ALTAIR_DEBUG
        printf("WebSocket max connections reached, rejecting %u\n", conn_id);
#endif
        server.close(conn_id);
        return;
    }

    ++g_ws_active_clients;
#ifdef ALTAIR_DEBUG
    printf("WebSocket client connected (id=%u, total=%zu)\n", conn_id, g_ws_active_clients);
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

    // Only decrement if this connection was tracked as active.
    ws_connection_state_t* conn = find_connection(conn_id);
    if (conn)
    {
        conn->active = false;
        conn->closing = false;

        if (g_ws_active_clients > 0)
        {
            --g_ws_active_clients;
        }
    }

#ifdef ALTAIR_DEBUG
    printf("WebSocket client closed (id=%u, remaining=%zu)\n", conn_id, g_ws_active_clients);
#endif

    ws_context_t* ctx = static_cast<ws_context_t*>(server.getCallbackExtra());
    if (ctx && ctx->callbacks.on_client_disconnected)
    {
        ctx->callbacks.on_client_disconnected(ctx->callbacks.user_data);
    }
}

void handle_message(WebSocketServer& server, uint32_t conn_id, const void* data, size_t len)
{
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
        ws_connection_state_t* conn = find_connection(conn_id);
        if (conn)
        {
            conn->closing = true;
        }
        server.close(conn_id);
    }
}

void handle_pong(WebSocketServer& server, uint32_t conn_id, const void* data, size_t len)
{
    (void)server;
    (void)data;
    (void)len;

    ws_connection_state_t* conn = find_connection(conn_id);
    if (!conn)
    {
        return;
    }

    if (conn->closing)
    {
        return;
    }

    conn->pending_pings = 0;
    conn->missed_pongs = 0;
    conn->next_ping_deadline = make_timeout_time_ms(WS_PING_INTERVAL_MS);
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

        g_ws_active_clients = 0;
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
        return g_ws_active_clients > 0;
    }

    void ws_poll_incoming(void)
    {
        if (!g_ws_running || !g_ws_server)
        {
            return;
        }

        // Always process messages even if no active clients (handles close race conditions)
        g_ws_server->popMessages();

        if (g_ws_active_clients == 0)
        {
            return;
        }

        // Heartbeat: check timers frequently (called every poll loop), so each client
        // pings relative to its own connect time rather than bunching on ws_poll_outgoing cadence.
        send_ping_if_due();
    }

    void ws_poll_outgoing(void)
    {
        if (!g_ws_running || !g_ws_server)
        {
            return;
        }

        if (g_ws_active_clients == 0 || !g_ws_context.callbacks.on_output)
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

        // Send to each active client individually (best-effort: don't block on slow clients)
        for (size_t i = 0; i < WS_MAX_CLIENTS; ++i)
        {
            ws_connection_state_t* conn = &g_ws_connections[i];
            if (!conn->active || conn->closing)
            {
                continue;
            }
#ifdef ALTAIR_DEBUG
            printf("WebSocket sending %zu bytes to %u\n", payload_len, conn->conn_id);
#endif

            if (!g_ws_server->sendMessage(conn->conn_id, payload, payload_len))
            {
#ifdef ALTAIR_DEBUG
                // This specific client's TCP send buffer is full - they miss this frame
                // (expected under load; real-time data, no backpressure)
                printf("WebSocket send failed, dropping %zu bytes\n", payload_len);
#endif
            }
        }
    }
}
