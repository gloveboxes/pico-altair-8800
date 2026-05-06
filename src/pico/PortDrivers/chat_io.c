#include "chat_io.h"

#include "pico/stdlib.h"

#if defined(CYW43_WL_GPIO_LED_PIN)

#include "../config.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/altcp.h"
#include "lwip/altcp_tls.h"
#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/pbuf.h"
#include "mbedtls/platform_time.h"
#include "mbedtls/ssl.h"
#include "pico/cyw43_arch.h"
#include "pico/util/queue.h"

#define CHAT_HOST "api.openai.com"
#define CHAT_PORT 443
#define CHAT_PATH "/v1/chat/completions"

#define CHAT_API_KEY_MAX 192
#define CHAT_REQUEST_MAX 8192
#define CHAT_RX_BUFFER_SIZE 8192
#define CHAT_SSE_LINE_MAX 1024
#define CHAT_RESPONSE_QUEUE_DEPTH 2048
#define CHAT_CONNECT_TIMEOUT_MS 15000
#define CHAT_STREAM_TIMEOUT_MS 120000

typedef struct
{
    uint32_t generation;
    size_t len;
    char json[CHAT_REQUEST_MAX];
} chat_request_t;

typedef enum
{
    CHAT_RESP_CHAR = 0,
    CHAT_RESP_EOF
} chat_response_type_t;

typedef struct
{
    uint32_t generation;
    chat_response_type_t type;
    uint8_t data;
} chat_response_t;

typedef enum
{
    CHAT_STATE_IDLE = 0,
    CHAT_STATE_DNS,
    CHAT_STATE_CONNECTING,
    CHAT_STATE_SENDING,
    CHAT_STATE_STREAMING
} chat_state_t;

typedef enum
{
    CHUNK_SIZE = 0,
    CHUNK_SIZE_LF,
    CHUNK_DATA,
    CHUNK_DATA_CR,
    CHUNK_DATA_LF
} chunk_state_t;

static queue_t chat_request_queue;
static queue_t chat_response_queue;
static char chat_api_key[CHAT_API_KEY_MAX];
static chat_request_t port_request_buffer;
static bool chat_network_available;

static struct
{
    char request[CHAT_REQUEST_MAX];
    size_t request_len;
    uint32_t generation;
    uint32_t next_generation;
    bool request_overflow;
    bool eof_seen;
    uint8_t pending_char;
    bool has_pending_char;
} port_state;

static struct
{
    chat_state_t state;
    struct altcp_pcb* pcb;
    struct altcp_tls_config* tls_config;
    altcp_allocator_t tls_allocator;
    ip_addr_t remote_addr;
    chat_request_t current_request;
    char header[512];
    size_t header_len;
    size_t header_sent;
    size_t body_sent;
    uint8_t rx_buffer[CHAT_RX_BUFFER_SIZE];
    size_t rx_len;
    volatile bool connected;
    volatile bool disconnected;
    volatile bool data_ready;
    volatile bool sent_ready;
    volatile const char* error_msg;
    uint32_t op_start_ms;
    uint32_t last_rx_ms;
    bool headers_done;
    uint32_t header_match;
    bool status_checked;
    int status_code;
    char status_line[40];
    size_t status_line_len;
    chunk_state_t chunk_state;
    size_t chunk_size;
    size_t chunk_read;
    bool chunk_extension;
    char sse_line[CHAT_SSE_LINE_MAX];
    size_t sse_len;
    bool response_truncated;
} client;

static void chat_start_dns(void);
static void chat_connect(void);
static void chat_send_more(void);
static void chat_cleanup(void);
static void chat_process_rx(void);
static void chat_process_body_byte(uint8_t ch);
static void chat_process_sse_byte(uint8_t ch);
static void chat_extract_content(const char* json);
static void chat_queue_char(uint32_t generation, uint8_t data, bool force);
static void chat_queue_text(uint32_t generation, const char* text);
static void chat_queue_eof(uint32_t generation);
static void chat_finish_ok(void);
static void chat_finish_error(const char* message);

static uint32_t chat_ms(void)
{
    return to_ms_since_boot(get_absolute_time());
}

mbedtls_ms_time_t mbedtls_ms_time(void)
{
    return (mbedtls_ms_time_t)chat_ms();
}

void chat_io_init(void)
{
    queue_init(&chat_request_queue, sizeof(chat_request_t), 1);
    queue_init(&chat_response_queue, sizeof(chat_response_t), CHAT_RESPONSE_QUEUE_DEPTH);
    memset(&port_state, 0, sizeof(port_state));
    port_state.next_generation = 1;
    memset(&client, 0, sizeof(client));
    client.state = CHAT_STATE_IDLE;
    chat_network_available = false;
    printf("[Chat] OpenAI chat port driver initialized on ports 120-124\n");
}

void chat_io_set_network_available(bool available)
{
    chat_network_available = available;
}

void chat_io_prompt_api_key(void)
{
    /* Try to load a previously stored key from flash first. */
    if (config_load_openai_key(chat_api_key, sizeof(chat_api_key)) && chat_api_key[0] != '\0')
    {
        printf("OpenAI API key loaded from flash (press 'K' within 5 seconds to replace).\n");

        absolute_time_t start = get_absolute_time();
        int c = PICO_ERROR_TIMEOUT;
        while (absolute_time_diff_us(start, get_absolute_time()) < 5000000)
        {
            c = getchar_timeout_us(100000);
            if (c == 'K' || c == 'k')
            {
                break;
            }
            if (c != PICO_ERROR_TIMEOUT)
            {
                /* Any other key: keep stored key. */
                return;
            }
        }
        if (c != 'K' && c != 'k')
        {
            return;
        }
        /* Fall through to re-prompt. */
        chat_api_key[0] = '\0';
    }

    if (!stdio_usb_connected())
    {
        /* Give the user up to 10 seconds to attach a serial terminal
         * before we give up on first-time API key entry. */
        absolute_time_t wait_start = get_absolute_time();
        while (!stdio_usb_connected() &&
               absolute_time_diff_us(wait_start, get_absolute_time()) < 10000000)
        {
            sleep_ms(100);
        }
        if (!stdio_usb_connected())
        {
            return;
        }
        sleep_ms(500);
    }

    printf("\n");
    printf("==============================================================\n");
    printf("  OpenAI API key not configured.\n");
    printf("  Paste key now, or press Enter to skip.\n");
    printf("  Boot will continue automatically after 30 seconds of idle.\n");
    printf("==============================================================\n");
    printf("openai> ");

    /* Wait up to 30 seconds for the first character. */
    absolute_time_t start = get_absolute_time();
    int c = PICO_ERROR_TIMEOUT;
    while (absolute_time_diff_us(start, get_absolute_time()) < 30000000)
    {
        c = getchar_timeout_us(100000);
        if (c != PICO_ERROR_TIMEOUT)
        {
            break;
        }
    }

    if (c == PICO_ERROR_TIMEOUT || c == '\r' || c == '\n')
    {
        printf("\nOpenAI API key not set. Chat API calls will return an error.\n");
        return;
    }

    size_t len = 0;
    /* Allow up to 30 seconds between subsequent characters (paste-friendly). */
    while (c != PICO_ERROR_TIMEOUT && c != '\r' && c != '\n')
    {
        if (len + 1 < sizeof(chat_api_key) && c >= 32 && c <= 126)
        {
            chat_api_key[len++] = (char)c;
            putchar('*');
        }
        c = getchar_timeout_us(30000000);
    }
    chat_api_key[len] = '\0';
    printf("\nOpenAI API key %s.\n", len > 0 ? "captured" : "not set");

    if (len > 0)
    {
        config_save_openai_key(chat_api_key);
    }
}

static void chat_reset_request(void)
{
    port_state.request_len = 0;
    port_state.request[0] = '\0';
    port_state.request_overflow = false;
}

static void chat_reset_response(void)
{
    chat_response_t discarded;
    while (queue_try_remove(&chat_response_queue, &discarded))
    {
    }
    port_state.eof_seen = false;
    port_state.has_pending_char = false;
    port_state.generation = port_state.next_generation++;
}

static void chat_request_add_char(uint8_t data)
{
    if (data == 0)
    {
        if (port_state.request_len < sizeof(port_state.request))
        {
            port_state.request[port_state.request_len] = '\0';
        }
        return;
    }

    if (port_state.request_len + 1 < sizeof(port_state.request))
    {
        port_state.request[port_state.request_len++] = (char)data;
        port_state.request[port_state.request_len] = '\0';
    }
    else
    {
        port_state.request_overflow = true;
    }
}

static void chat_trigger_request(void)
{
    chat_reset_response();

    if (port_state.request_overflow || port_state.request_len == 0)
    {
        chat_queue_text(port_state.generation, "OpenAI request buffer error\n");
        chat_queue_eof(port_state.generation);
        return;
    }

    memset(&port_request_buffer, 0, sizeof(port_request_buffer));
    port_request_buffer.generation = port_state.generation;
    port_request_buffer.len = port_state.request_len;
    memcpy(port_request_buffer.json, port_state.request, port_state.request_len + 1);

    if (!queue_try_add(&chat_request_queue, &port_request_buffer))
    {
        chat_queue_text(port_state.generation, "OpenAI request queue busy\n");
        chat_queue_eof(port_state.generation);
    }
}

size_t chat_output(int port, uint8_t data, char* buffer, size_t buffer_length)
{
    (void)buffer;
    (void)buffer_length;

    if (port == CHAT_PORT_TRIGGER)
    {
        chat_reset_request();
    }
    else if (port == CHAT_PORT_REQUEST)
    {
        chat_request_add_char(data);
    }
    else if (port == CHAT_PORT_RESET_RESPONSE)
    {
        chat_reset_response();
    }

    return 0;
}

static bool chat_load_next_char(void)
{
    chat_response_t response;

    while (queue_try_remove(&chat_response_queue, &response))
    {
        if (response.generation != port_state.generation)
        {
            continue;
        }
        if (response.type == CHAT_RESP_EOF)
        {
            port_state.eof_seen = true;
            return false;
        }
        port_state.pending_char = response.data;
        port_state.has_pending_char = true;
        return true;
    }

    return false;
}

uint8_t chat_input(uint8_t port)
{
    if (port == CHAT_PORT_TRIGGER)
    {
        chat_trigger_request();
        return 0;
    }

    if (port == CHAT_PORT_STATUS)
    {
        if (port_state.has_pending_char || chat_load_next_char())
        {
            return CHAT_STATUS_DATA_READY;
        }
        return port_state.eof_seen ? CHAT_STATUS_EOF : CHAT_STATUS_WAITING;
    }

    if (port == CHAT_PORT_DATA)
    {
        if (!port_state.has_pending_char)
        {
            chat_load_next_char();
        }
        if (port_state.has_pending_char)
        {
            port_state.has_pending_char = false;
            return port_state.pending_char;
        }
    }

    return 0;
}

static void chat_dns_found(const char* name, const ip_addr_t* ipaddr, void* arg)
{
    (void)name;
    (void)arg;
    if (ipaddr != NULL)
    {
        client.remote_addr = *ipaddr;
        client.connected = true;
    }
    else
    {
        client.error_msg = "OpenAI DNS lookup failed";
        client.disconnected = true;
    }
}

static err_t chat_connected_cb(void* arg, struct altcp_pcb* pcb, err_t err)
{
    (void)arg;
    (void)pcb;
    if (err == ERR_OK)
    {
        client.connected = true;
    }
    else
    {
        client.error_msg = "OpenAI TLS connection failed";
        client.disconnected = true;
    }
    return ERR_OK;
}

static void chat_err_cb(void* arg, err_t err)
{
    (void)arg;
    (void)err;
    client.pcb = NULL;
    client.error_msg = "OpenAI connection error";
    client.disconnected = true;
}

static err_t chat_recv_cb(void* arg, struct altcp_pcb* pcb, struct pbuf* p, err_t err)
{
    (void)arg;

    if (err != ERR_OK || p == NULL)
    {
        if (p)
        {
            pbuf_free(p);
        }
        client.disconnected = true;
        return ERR_OK;
    }

    size_t copy_len = p->tot_len;
    if (copy_len > sizeof(client.rx_buffer) - client.rx_len)
    {
        copy_len = sizeof(client.rx_buffer) - client.rx_len;
        client.error_msg = "OpenAI receive buffer overflow";
        client.disconnected = true;
    }

    if (copy_len > 0)
    {
        pbuf_copy_partial(p, &client.rx_buffer[client.rx_len], copy_len, 0);
        client.rx_len += copy_len;
        client.data_ready = true;
    }

    altcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t chat_sent_cb(void* arg, struct altcp_pcb* pcb, u16_t len)
{
    (void)arg;
    (void)pcb;
    (void)len;
    client.sent_ready = true;
    return ERR_OK;
}

void chat_client_poll(void)
{
    /* Drain any pending RX before noticing a disconnect, so a final
     * batch containing [DONE] (or chunked terminator) is parsed and
     * completes the request cleanly. */
    if (client.data_ready)
    {
        client.data_ready = false;
        chat_process_rx();
    }

    if (client.disconnected)
    {
        client.disconnected = false;
        const char* msg = client.error_msg ? (const char*)client.error_msg : "OpenAI stream closed";
        client.error_msg = NULL;
        chat_finish_error(msg);
        return;
    }

    if (client.state == CHAT_STATE_DNS && client.connected)
    {
        client.connected = false;
        chat_connect();
        return;
    }

    if (client.state == CHAT_STATE_CONNECTING && client.connected)
    {
        client.connected = false;
        client.state = CHAT_STATE_SENDING;
        client.sent_ready = true;
        chat_send_more();
        return;
    }

    if (client.data_ready)
    {
        client.data_ready = false;
        chat_process_rx();
        if (client.state == CHAT_STATE_IDLE)
        {
            return;
        }
    }

    if (client.state == CHAT_STATE_SENDING && client.sent_ready)
    {
        client.sent_ready = false;
        chat_send_more();
    }

    uint32_t now = chat_ms();
    if ((client.state == CHAT_STATE_DNS || client.state == CHAT_STATE_CONNECTING) &&
        now - client.op_start_ms > CHAT_CONNECT_TIMEOUT_MS)
    {
        chat_finish_error("OpenAI connection timeout");
        return;
    }

    if (client.state == CHAT_STATE_STREAMING && now - client.last_rx_ms > CHAT_STREAM_TIMEOUT_MS)
    {
        chat_finish_error("OpenAI stream timeout");
        return;
    }

    if (client.state == CHAT_STATE_IDLE)
    {
        chat_request_t request;
        if (queue_try_remove(&chat_request_queue, &request))
        {
            client.current_request = request;
            if (chat_api_key[0] == '\0')
            {
                chat_queue_text(request.generation, "OpenAI API key not configured\n");
                chat_queue_eof(request.generation);
            }
            else if (!chat_network_available)
            {
                chat_queue_text(request.generation, "OpenAI network unavailable\n");
                chat_queue_eof(request.generation);
            }
            else
            {
                chat_start_dns();
            }
        }
    }
}

static void chat_start_dns(void)
{
    client.rx_len = 0;
    client.headers_done = false;
    client.header_match = 0;
    client.status_checked = false;
    client.status_code = 0;
    client.status_line_len = 0;
    client.chunk_state = CHUNK_SIZE;
    client.chunk_size = 0;
    client.chunk_read = 0;
    client.chunk_extension = false;
    client.sse_len = 0;
    client.response_truncated = false;
    client.connected = false;
    client.disconnected = false;
    client.data_ready = false;
    client.sent_ready = false;
    client.op_start_ms = chat_ms();
    client.last_rx_ms = client.op_start_ms;
    client.state = CHAT_STATE_DNS;

    cyw43_arch_lwip_begin();
    err_t err = dns_gethostbyname(CHAT_HOST, &client.remote_addr, chat_dns_found, NULL);
    cyw43_arch_lwip_end();

    if (err == ERR_OK)
    {
        chat_connect();
    }
    else if (err == ERR_INPROGRESS)
    {
        /* DNS callback will advance the state. */
    }
    else
    {
        chat_finish_error("OpenAI DNS lookup failed");
    }
}

static void chat_connect(void)
{
    client.state = CHAT_STATE_CONNECTING;
    client.op_start_ms = chat_ms();

    if (client.tls_config == NULL)
    {
        client.tls_config = altcp_tls_create_config_client(NULL, 0);
        client.tls_allocator.alloc = altcp_tls_alloc;
        client.tls_allocator.arg = client.tls_config;
    }

    if (client.tls_config == NULL)
    {
        chat_finish_error("OpenAI TLS config failed");
        return;
    }

    cyw43_arch_lwip_begin();
    client.pcb = altcp_new(&client.tls_allocator);
    if (client.pcb == NULL)
    {
        cyw43_arch_lwip_end();
        chat_finish_error("OpenAI PCB allocation failed");
        return;
    }

    altcp_arg(client.pcb, NULL);
    altcp_recv(client.pcb, chat_recv_cb);
    altcp_sent(client.pcb, chat_sent_cb);
    altcp_err(client.pcb, chat_err_cb);
    altcp_nagle_disable(client.pcb);

    mbedtls_ssl_context* ssl = (mbedtls_ssl_context*)altcp_tls_context(client.pcb);
    if (ssl != NULL)
    {
        mbedtls_ssl_set_hostname(ssl, CHAT_HOST);
    }

    err_t err = altcp_connect(client.pcb, &client.remote_addr, CHAT_PORT, chat_connected_cb);
    cyw43_arch_lwip_end();

    if (err != ERR_OK)
    {
        chat_finish_error("OpenAI connect failed");
        return;
    }
}

static void chat_send_more(void)
{
    if (client.header_len == 0)
    {
        client.header_len = (size_t)snprintf(client.header, sizeof(client.header),
            "POST " CHAT_PATH " HTTP/1.1\r\n"
            "Host: " CHAT_HOST "\r\n"
            "Authorization: Bearer %s\r\n"
            "Content-Type: application/json\r\n"
            "Accept: text/event-stream\r\n"
            "Connection: close\r\n"
            "Content-Length: %u\r\n\r\n",
            chat_api_key, (unsigned int)client.current_request.len);
        client.header_sent = 0;
        client.body_sent = 0;
    }

    while (client.header_sent < client.header_len || client.body_sent < client.current_request.len)
    {
        const uint8_t* data;
        size_t remaining;

        if (client.header_sent < client.header_len)
        {
            data = (const uint8_t*)&client.header[client.header_sent];
            remaining = client.header_len - client.header_sent;
        }
        else
        {
            data = (const uint8_t*)&client.current_request.json[client.body_sent];
            remaining = client.current_request.len - client.body_sent;
        }

        cyw43_arch_lwip_begin();
        u16_t available = altcp_sndbuf(client.pcb);
        if (available == 0)
        {
            cyw43_arch_lwip_end();
            break;
        }
        if (remaining > available)
        {
            remaining = available;
        }
        if (remaining > 1024)
        {
            remaining = 1024;
        }

        err_t err = altcp_write(client.pcb, data, (u16_t)remaining, TCP_WRITE_FLAG_COPY);
        if (err == ERR_OK)
        {
            altcp_output(client.pcb);
        }
        cyw43_arch_lwip_end();

        if (err != ERR_OK)
        {
            chat_finish_error("OpenAI send failed");
            return;
        }

        if (client.header_sent < client.header_len)
        {
            client.header_sent += remaining;
        }
        else
        {
            client.body_sent += remaining;
        }
    }

    if (client.header_sent >= client.header_len && client.body_sent >= client.current_request.len)
    {
        client.state = CHAT_STATE_STREAMING;
        client.last_rx_ms = chat_ms();
    }
}

static int chat_hex_value(uint8_t ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static void chat_process_rx(void)
{
    static uint8_t rx_snapshot[CHAT_RX_BUFFER_SIZE];

    /* Copy rx state under the lwIP lock, then parse outside it. Completion
     * paths abort/cleanup the pcb and must be able to take the lwIP lock. */
    cyw43_arch_lwip_begin();
    size_t len = client.rx_len;
    if (len > sizeof(rx_snapshot))
    {
        len = sizeof(rx_snapshot);
    }
    if (len > 0)
    {
        memcpy(rx_snapshot, client.rx_buffer, len);
    }
    client.rx_len = 0;
    client.last_rx_ms = chat_ms();
    cyw43_arch_lwip_end();

    for (size_t i = 0; i < len; i++)
    {
        if (client.state == CHAT_STATE_IDLE)
        {
            /* finish_ok/error already ran during this batch — discard the
             * rest so it isn't fed to the next request's parser. */
            break;
        }
        uint8_t ch = rx_snapshot[i];
        if (!client.headers_done)
        {
            if (!client.status_checked)
            {
                if (ch == '\r')
                {
                    client.status_line[client.status_line_len] = '\0';
                    client.status_checked = true;
                    /* Expected: "HTTP/1.1 200 OK" */
                    const char* sp = strchr(client.status_line, ' ');
                    if (sp != NULL)
                    {
                        client.status_code = atoi(sp + 1);
                    }
                    if (client.status_code != 200)
                    {
                        static char status_err[48];
                        snprintf(status_err, sizeof(status_err),
                            "OpenAI HTTP %d", client.status_code);
                        client.error_msg = status_err;
                        client.disconnected = true;
                        break;
                    }
                }
                else if (client.status_line_len + 1 < sizeof(client.status_line))
                {
                    client.status_line[client.status_line_len++] = (char)ch;
                }
            }
            client.header_match = (client.header_match << 8) | ch;
            if (client.header_match == 0x0d0a0d0au)
            {
                client.headers_done = true;
            }
            continue;
        }
        chat_process_body_byte(ch);
    }
}

static void chat_process_body_byte(uint8_t ch)
{
    switch (client.chunk_state)
    {
        case CHUNK_SIZE:
        {
            int value = chat_hex_value(ch);
            if (value >= 0 && !client.chunk_extension)
            {
                client.chunk_size = (client.chunk_size << 4) | (size_t)value;
            }
            else if (ch == ';')
            {
                client.chunk_extension = true;
            }
            else if (ch == '\r')
            {
                client.chunk_state = CHUNK_SIZE_LF;
            }
            break;
        }

        case CHUNK_SIZE_LF:
            if (ch == '\n')
            {
                if (client.chunk_size == 0)
                {
                    chat_finish_ok();
                }
                else
                {
                    client.chunk_read = 0;
                    client.chunk_state = CHUNK_DATA;
                }
            }
            break;

        case CHUNK_DATA:
            chat_process_sse_byte(ch);
            client.chunk_read++;
            if (client.chunk_read >= client.chunk_size)
            {
                client.chunk_state = CHUNK_DATA_CR;
            }
            break;

        case CHUNK_DATA_CR:
            client.chunk_state = CHUNK_DATA_LF;
            break;

        case CHUNK_DATA_LF:
            client.chunk_size = 0;
            client.chunk_read = 0;
            client.chunk_extension = false;
            client.chunk_state = CHUNK_SIZE;
            break;
    }
}

static void chat_process_sse_byte(uint8_t ch)
{
    if (ch == '\r')
    {
        return;
    }

    if (ch != '\n')
    {
        if (client.sse_len + 1 < sizeof(client.sse_line))
        {
            client.sse_line[client.sse_len++] = (char)ch;
        }
        return;
    }

    client.sse_line[client.sse_len] = '\0';
    if (strncmp(client.sse_line, "data: ", 6) == 0)
    {
        const char* payload = client.sse_line + 6;
        if (strcmp(payload, "[DONE]") == 0)
        {
            chat_finish_ok();
        }
        else
        {
            chat_extract_content(payload);
        }
    }
    client.sse_len = 0;
}

static int chat_hex_nibble(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static void chat_emit_json_string(const char* ptr)
{
    while (*ptr && *ptr != '"')
    {
        uint8_t out = (uint8_t)*ptr++;
        if (out == '\\' && *ptr)
        {
            char esc = *ptr++;
            switch (esc)
            {
                case 'n': out = '\n'; break;
                case 'r': out = '\r'; break;
                case 't': out = '\t'; break;
                case '"': out = '"'; break;
                case '\\': out = '\\'; break;
                case 'u':
                    if (chat_hex_nibble(ptr[0]) >= 0 && chat_hex_nibble(ptr[1]) >= 0 &&
                        chat_hex_nibble(ptr[2]) >= 0 && chat_hex_nibble(ptr[3]) >= 0)
                    {
                        ptr += 4;
                    }
                    out = '?';
                    break;
                default:
                    out = (uint8_t)esc;
                    break;
            }
        }
        chat_queue_char(client.current_request.generation, out & 0x7f, false);
    }
}

static void chat_extract_content(const char* json)
{
    const char* ptr = json;
    const char* marker = "\"content\":\"";
    size_t marker_len = strlen(marker);

    while ((ptr = strstr(ptr, marker)) != NULL)
    {
        ptr += marker_len;
        chat_emit_json_string(ptr);
    }
}

static void chat_queue_response(const chat_response_t* response, bool force)
{
    if (queue_try_add(&chat_response_queue, response))
    {
        return;
    }

    client.response_truncated = true;
    if (!force)
    {
        return;
    }

    chat_response_t discarded;
    while (!queue_try_add(&chat_response_queue, response))
    {
        if (!queue_try_remove(&chat_response_queue, &discarded))
        {
            return;
        }
    }
}

static void chat_queue_char(uint32_t generation, uint8_t data, bool force)
{
    chat_response_t response;
    response.generation = generation;
    response.type = CHAT_RESP_CHAR;
    response.data = data;
    chat_queue_response(&response, force);
}

static void chat_queue_text(uint32_t generation, const char* text)
{
    while (*text)
    {
        chat_queue_char(generation, (uint8_t)(*text++ & 0x7f), false);
    }
}

static void chat_queue_text_force(uint32_t generation, const char* text)
{
    while (*text)
    {
        chat_queue_char(generation, (uint8_t)(*text++ & 0x7f), true);
    }
}

static void chat_queue_eof(uint32_t generation)
{
    if (client.response_truncated)
    {
        client.response_truncated = false;
        chat_queue_text_force(generation, "\nOpenAI response truncated\n");
    }

    chat_response_t response;
    response.generation = generation;
    response.type = CHAT_RESP_EOF;
    response.data = 0;
    chat_queue_response(&response, true);
}

static void chat_cleanup(void)
{
    if (client.pcb != NULL)
    {
        cyw43_arch_lwip_begin();
        altcp_abort(client.pcb);
        cyw43_arch_lwip_end();
        client.pcb = NULL;
    }
    client.header_len = 0;
    client.header_sent = 0;
    client.body_sent = 0;
    client.rx_len = 0;
}

static void chat_finish_ok(void)
{
    if (client.state == CHAT_STATE_IDLE)
    {
        return;
    }
    chat_queue_eof(client.current_request.generation);
    chat_cleanup();
    client.state = CHAT_STATE_IDLE;
}

static void chat_finish_error(const char* message)
{
    if (client.state == CHAT_STATE_IDLE)
    {
        /* Already finished (e.g. clean [DONE] followed by server FIN).
         * Don't queue a phantom error onto a completed generation. */
        chat_cleanup();
        return;
    }
    uint32_t generation = client.current_request.generation;
    if (generation == 0)
    {
        generation = port_state.generation;
    }
    if (message != NULL && *message != '\0')
    {
        chat_queue_text(generation, message);
        chat_queue_char(generation, '\n', false);
    }
    chat_queue_eof(generation);
    chat_cleanup();
    client.state = CHAT_STATE_IDLE;
}

#else

#include <stddef.h>
#include <stdint.h>

void chat_io_init(void) {}
void chat_io_prompt_api_key(void) {}
void chat_io_set_network_available(bool available) { (void)available; }

size_t chat_output(int port, uint8_t data, char* buffer, size_t buffer_length)
{
    (void)port;
    (void)data;
    (void)buffer;
    (void)buffer_length;
    return 0;
}

uint8_t chat_input(uint8_t port)
{
    (void)port;
    return CHAT_STATUS_EOF;
}

void chat_client_poll(void) {}

#endif