#include "ws.h"

#include "pico_ws_server/web_socket_server.h"

#include <cstdio>
#include <cstring>
#include <memory>

namespace
{
static constexpr uint16_t WS_SERVER_PORT = 8082;
static constexpr uint32_t WS_MAX_CLIENTS = 1;
static constexpr size_t WS_FRAME_PAYLOAD = 4;
static constexpr size_t WS_MAX_FRAMES_PER_POLL = 8;

struct ws_context_t
{
    ws_callbacks_t callbacks;
};

static ws_context_t g_ws_context = {};
static bool g_ws_initialized = false;
static bool g_ws_running = false;
static size_t g_ws_active_clients = 0;
static std::unique_ptr<WebSocketServer> g_ws_server;

void handle_connect(WebSocketServer &server, uint32_t conn_id)
{
    ++g_ws_active_clients;
    printf("WebSocket client connected (id=%u)\n", conn_id);

    ws_context_t *ctx = static_cast<ws_context_t *>(server.getCallbackExtra());
    if (ctx && ctx->callbacks.on_client_connected)
    {
        ctx->callbacks.on_client_connected(ctx->callbacks.user_data);
    }
}

void handle_close(WebSocketServer &server, uint32_t conn_id)
{
    if (g_ws_active_clients > 0)
    {
        --g_ws_active_clients;
    }
    printf("WebSocket client closed (id=%u)\n", conn_id);

    ws_context_t *ctx = static_cast<ws_context_t *>(server.getCallbackExtra());
    if (ctx && ctx->callbacks.on_client_disconnected)
    {
        ctx->callbacks.on_client_disconnected(ctx->callbacks.user_data);
    }
}

void handle_message(WebSocketServer &server, uint32_t conn_id, const void *data, size_t len)
{
    ws_context_t *ctx = static_cast<ws_context_t *>(server.getCallbackExtra());
    if (!ctx)
    {
        return;
    }

    bool keep_open = true;
    if (ctx->callbacks.on_receive)
    {
        keep_open = ctx->callbacks.on_receive(static_cast<const uint8_t *>(data), len, ctx->callbacks.user_data);
    }

    if (!keep_open)
    {
        server.close(conn_id);
    }
}
} // namespace

extern "C"
{

void ws_init(const ws_callbacks_t *callbacks)
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
        printf("WebSocket server not initialized\n");
        return false;
    }

    if (g_ws_running)
    {
        return true;
    }

    if (!g_ws_server)
    {
        g_ws_server = std::make_unique<WebSocketServer>(WS_MAX_CLIENTS);
        g_ws_server->setCallbackExtra(&g_ws_context);
        g_ws_server->setConnectCallback(handle_connect);
        g_ws_server->setCloseCallback(handle_close);
        g_ws_server->setMessageCallback(handle_message);
    }

    g_ws_active_clients = 0;
    if (!g_ws_server->startListening(WS_SERVER_PORT))
    {
        printf("Failed to start WebSocket server on port %u\n", WS_SERVER_PORT);
        g_ws_server.reset();
        g_ws_running = false;
        return false;
    }

    g_ws_running = true;
    printf("WebSocket server listening on port %u\n", WS_SERVER_PORT);
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

void ws_poll(void)
{
    if (!g_ws_running || !g_ws_server)
    {
        return;
    }

    g_ws_server->popMessages();

    if (g_ws_active_clients == 0 || !g_ws_context.callbacks.on_output)
    {
        return;
    }

    uint8_t payload[WS_FRAME_PAYLOAD];

    for (size_t frame = 0; frame < WS_MAX_FRAMES_PER_POLL; ++frame)
    {
        size_t payload_len = g_ws_context.callbacks.on_output(payload, sizeof(payload), g_ws_context.callbacks.user_data);
        if (payload_len == 0)
        {
            break;
        }
        g_ws_server->broadcastMessage(payload, payload_len);
    }
}

}