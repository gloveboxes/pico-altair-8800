/**
 * @file captive_portal.c
 * @brief WiFi AP mode captive portal implementation
 */

#include "captive_portal.h"
#include "config_page_hex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/watchdog.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"

#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"

#include "config.h"

// ============================================================================
// DHCP Server (adapted from btstack's dhserver)
// ============================================================================

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

#define DHCP_DISCOVER 1
#define DHCP_OFFER 2
#define DHCP_REQUEST 3
#define DHCP_ACK 5

// DHCP options
#define DHCP_OPT_PAD 0
#define DHCP_OPT_SUBNET_MASK 1
#define DHCP_OPT_ROUTER 3
#define DHCP_OPT_DNS 6
#define DHCP_OPT_REQUESTED_IP 50
#define DHCP_OPT_LEASE_TIME 51
#define DHCP_OPT_MSG_TYPE 53
#define DHCP_OPT_SERVER_ID 54
#define DHCP_OPT_END 255

typedef struct __attribute__((packed))
{
    uint8_t op;
    uint8_t htype;
    uint8_t hlen;
    uint8_t hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint8_t ciaddr[4];
    uint8_t yiaddr[4];
    uint8_t siaddr[4];
    uint8_t giaddr[4];
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint8_t magic[4];
    uint8_t options[312];
} dhcp_msg_t;

// DHCP lease table
#define DHCP_MAX_LEASES 4
typedef struct
{
    uint8_t mac[6];
    uint8_t ip[4];
    bool active;
} dhcp_lease_t;

static dhcp_lease_t dhcp_leases[DHCP_MAX_LEASES];
static struct udp_pcb* dhcp_pcb = NULL;

// Our AP IP address
static uint8_t ap_ip[4] = {192, 168, 4, 1};
static uint8_t ap_netmask[4] = {255, 255, 255, 0};
static uint8_t dhcp_pool_start[4] = {192, 168, 4, 10};

static const uint8_t dhcp_magic[] = {0x63, 0x82, 0x53, 0x63};

static uint8_t* dhcp_find_option(uint8_t* options, size_t len, uint8_t opt)
{
    size_t i = 0;
    while (i < len && options[i] != DHCP_OPT_END)
    {
        if (options[i] == DHCP_OPT_PAD)
        {
            i++;
            continue;
        }
        if (i + 1 >= len)
            break;
        uint8_t opt_len = options[i + 1];
        if (options[i] == opt)
            return &options[i];
        i += 2 + opt_len;
    }
    return NULL;
}

static dhcp_lease_t* dhcp_find_lease_by_mac(const uint8_t* mac)
{
    for (int i = 0; i < DHCP_MAX_LEASES; i++)
    {
        if (dhcp_leases[i].active && memcmp(dhcp_leases[i].mac, mac, 6) == 0)
        {
            return &dhcp_leases[i];
        }
    }
    return NULL;
}

static dhcp_lease_t* dhcp_allocate_lease(const uint8_t* mac)
{
    // First check if MAC already has a lease
    dhcp_lease_t* lease = dhcp_find_lease_by_mac(mac);
    if (lease)
        return lease;

    // Find free slot
    for (int i = 0; i < DHCP_MAX_LEASES; i++)
    {
        if (!dhcp_leases[i].active)
        {
            memcpy(dhcp_leases[i].mac, mac, 6);
            dhcp_leases[i].ip[0] = dhcp_pool_start[0];
            dhcp_leases[i].ip[1] = dhcp_pool_start[1];
            dhcp_leases[i].ip[2] = dhcp_pool_start[2];
            dhcp_leases[i].ip[3] = dhcp_pool_start[3] + i;
            dhcp_leases[i].active = true;
            return &dhcp_leases[i];
        }
    }
    return NULL;
}

static size_t dhcp_add_options(uint8_t* buf, uint8_t msg_type, const uint8_t* client_ip)
{
    uint8_t* p = buf;

    // Message type
    *p++ = DHCP_OPT_MSG_TYPE;
    *p++ = 1;
    *p++ = msg_type;

    // Server identifier
    *p++ = DHCP_OPT_SERVER_ID;
    *p++ = 4;
    memcpy(p, ap_ip, 4);
    p += 4;

    // Lease time (1 hour)
    *p++ = DHCP_OPT_LEASE_TIME;
    *p++ = 4;
    *p++ = 0x00;
    *p++ = 0x00;
    *p++ = 0x0e;
    *p++ = 0x10; // 3600 seconds

    // Subnet mask
    *p++ = DHCP_OPT_SUBNET_MASK;
    *p++ = 4;
    memcpy(p, ap_netmask, 4);
    p += 4;

    // Router (gateway)
    *p++ = DHCP_OPT_ROUTER;
    *p++ = 4;
    memcpy(p, ap_ip, 4);
    p += 4;

    // DNS server (point to ourselves for captive portal)
    *p++ = DHCP_OPT_DNS;
    *p++ = 4;
    memcpy(p, ap_ip, 4);
    p += 4;

    // End
    *p++ = DHCP_OPT_END;

    return p - buf;
}

static void dhcp_recv_cb(void* arg, struct udp_pcb* pcb, struct pbuf* p, const ip_addr_t* addr, u16_t port)
{
    (void)arg;
    (void)addr;

    if (p->tot_len < sizeof(dhcp_msg_t) - 312)
    {
        pbuf_free(p);
        return;
    }

    dhcp_msg_t msg;
    pbuf_copy_partial(p, &msg, sizeof(msg), 0);
    pbuf_free(p);

    // Verify magic cookie
    if (memcmp(msg.magic, dhcp_magic, 4) != 0)
        return;

    // Find message type option
    uint8_t* opt = dhcp_find_option(msg.options, sizeof(msg.options), DHCP_OPT_MSG_TYPE);
    if (!opt || opt[1] != 1)
        return;

    uint8_t msg_type = opt[2];
    dhcp_lease_t* lease = NULL;
    uint8_t response_type = 0;

    switch (msg_type)
    {
        case DHCP_DISCOVER:
            lease = dhcp_allocate_lease(msg.chaddr);
            if (lease)
            {
                response_type = DHCP_OFFER;
                printf("[Captive] DHCP DISCOVER from %02x:%02x:%02x:%02x:%02x:%02x -> OFFER %d.%d.%d.%d\n",
                       msg.chaddr[0], msg.chaddr[1], msg.chaddr[2], msg.chaddr[3], msg.chaddr[4], msg.chaddr[5],
                       lease->ip[0], lease->ip[1], lease->ip[2], lease->ip[3]);
            }
            break;

        case DHCP_REQUEST:
            lease = dhcp_find_lease_by_mac(msg.chaddr);
            if (lease)
            {
                response_type = DHCP_ACK;
                printf("[Captive] DHCP REQUEST from %02x:%02x:%02x:%02x:%02x:%02x -> ACK %d.%d.%d.%d\n",
                       msg.chaddr[0], msg.chaddr[1], msg.chaddr[2], msg.chaddr[3], msg.chaddr[4], msg.chaddr[5],
                       lease->ip[0], lease->ip[1], lease->ip[2], lease->ip[3]);
            }
            break;

        default:
            return;
    }

    if (!lease || response_type == 0)
        return;

    // Build response
    dhcp_msg_t reply;
    memset(&reply, 0, sizeof(reply));
    reply.op = 2; // BOOTREPLY
    reply.htype = 1;
    reply.hlen = 6;
    reply.xid = msg.xid;
    memcpy(reply.yiaddr, lease->ip, 4);
    memcpy(reply.siaddr, ap_ip, 4);
    memcpy(reply.chaddr, msg.chaddr, 16);
    memcpy(reply.magic, dhcp_magic, 4);

    size_t opt_len = dhcp_add_options(reply.options, response_type, lease->ip);

    // Send response
    struct pbuf* resp = pbuf_alloc(PBUF_TRANSPORT, sizeof(dhcp_msg_t), PBUF_RAM);
    if (resp)
    {
        memcpy(resp->payload, &reply, sizeof(reply));
        struct netif* nif = netif_default;
        if (nif)
        {
            udp_sendto_if(pcb, resp, IP_ADDR_BROADCAST, DHCP_CLIENT_PORT, nif);
        }
        pbuf_free(resp);
    }
}

static bool dhcp_server_start(void)
{
    memset(dhcp_leases, 0, sizeof(dhcp_leases));

    dhcp_pcb = udp_new();
    if (!dhcp_pcb)
    {
        printf("[Captive] Failed to create DHCP PCB\n");
        return false;
    }

    err_t err = udp_bind(dhcp_pcb, IP_ADDR_ANY, DHCP_SERVER_PORT);
    if (err != ERR_OK)
    {
        printf("[Captive] Failed to bind DHCP server: %d\n", err);
        udp_remove(dhcp_pcb);
        dhcp_pcb = NULL;
        return false;
    }

    udp_recv(dhcp_pcb, dhcp_recv_cb, NULL);
    printf("[Captive] DHCP server started on port %d\n", DHCP_SERVER_PORT);
    return true;
}

static void dhcp_server_stop(void)
{
    if (dhcp_pcb)
    {
        udp_remove(dhcp_pcb);
        dhcp_pcb = NULL;
    }
}

// ============================================================================
// DNS Server (Captive Portal - redirect all queries to our IP)
// ============================================================================

static struct udp_pcb* dns_pcb = NULL;

typedef struct __attribute__((packed))
{
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_header_t;

static void dns_recv_cb(void* arg, struct udp_pcb* pcb, struct pbuf* p, const ip_addr_t* addr, u16_t port)
{
    (void)arg;

    if (p->tot_len < sizeof(dns_header_t))
    {
        pbuf_free(p);
        return;
    }

    // Read the query
    uint8_t query[512];
    size_t query_len = pbuf_copy_partial(p, query, sizeof(query), 0);
    pbuf_free(p);

    if (query_len < sizeof(dns_header_t))
        return;

    dns_header_t* hdr = (dns_header_t*)query;

    // Only handle standard queries
    uint16_t flags = lwip_ntohs(hdr->flags);
    if ((flags & 0x8000) != 0)
        return; // This is a response, not a query

    // Build response - we'll redirect ALL queries to our IP
    uint8_t response[512];
    memcpy(response, query, query_len);

    dns_header_t* resp_hdr = (dns_header_t*)response;
    resp_hdr->flags = lwip_htons(0x8180);  // Response, No error
    resp_hdr->ancount = lwip_htons(1);     // One answer
    resp_hdr->nscount = 0;
    resp_hdr->arcount = 0;

    // Find the end of the question section
    size_t qname_start = sizeof(dns_header_t);
    size_t pos = qname_start;
    while (pos < query_len && query[pos] != 0)
    {
        pos += query[pos] + 1;
    }
    pos++; // Skip null terminator
    pos += 4; // Skip QTYPE and QCLASS

    // Add answer section
    size_t ans_start = pos;

    // Name pointer to question
    response[pos++] = 0xC0;
    response[pos++] = (uint8_t)qname_start;

    // Type A (1)
    response[pos++] = 0x00;
    response[pos++] = 0x01;

    // Class IN (1)
    response[pos++] = 0x00;
    response[pos++] = 0x01;

    // TTL (60 seconds)
    response[pos++] = 0x00;
    response[pos++] = 0x00;
    response[pos++] = 0x00;
    response[pos++] = 0x3C;

    // RDLENGTH (4 bytes for IPv4)
    response[pos++] = 0x00;
    response[pos++] = 0x04;

    // RDATA (our IP address)
    response[pos++] = ap_ip[0];
    response[pos++] = ap_ip[1];
    response[pos++] = ap_ip[2];
    response[pos++] = ap_ip[3];

    // Send response
    struct pbuf* resp = pbuf_alloc(PBUF_TRANSPORT, pos, PBUF_RAM);
    if (resp)
    {
        memcpy(resp->payload, response, pos);
        udp_sendto(pcb, resp, addr, port);
        pbuf_free(resp);
    }
}

static bool dns_server_start(void)
{
    dns_pcb = udp_new();
    if (!dns_pcb)
    {
        printf("[Captive] Failed to create DNS PCB\n");
        return false;
    }

    err_t err = udp_bind(dns_pcb, IP_ADDR_ANY, CAPTIVE_PORTAL_DNS_PORT);
    if (err != ERR_OK)
    {
        printf("[Captive] Failed to bind DNS server: %d\n", err);
        udp_remove(dns_pcb);
        dns_pcb = NULL;
        return false;
    }

    udp_recv(dns_pcb, dns_recv_cb, NULL);
    printf("[Captive] DNS server started (captive portal redirect)\n");
    return true;
}

static void dns_server_stop(void)
{
    if (dns_pcb)
    {
        udp_remove(dns_pcb);
        dns_pcb = NULL;
    }
}

// ============================================================================
// HTTP Server for Config Page
// ============================================================================

static struct tcp_pcb* http_pcb = NULL;
static bool portal_running = false;
static bool reboot_pending = false;
static absolute_time_t reboot_deadline;

#define HTTP_RECV_BUF_SIZE 1024

typedef struct
{
    struct tcp_pcb* pcb;
    char recv_buf[HTTP_RECV_BUF_SIZE];
    size_t recv_len;
    bool headers_complete;
    size_t content_length;
    bool is_post;
} http_conn_t;

// HTTP response headers
static const char HTTP_200_HTML_GZ[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html\r\n"
    "Content-Encoding: gzip\r\n"
    "Cache-Control: no-store, max-age=0\r\n"
    "Pragma: no-cache\r\n"
    "Connection: close\r\n"
    "Content-Length: ";

static const char HTTP_200_TEXT[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Connection: close\r\n"
    "\r\n";

static const char HTTP_200_JSON[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Cache-Control: no-store, max-age=0\r\n"
    "Pragma: no-cache\r\n"
    "Connection: close\r\n"
    "Content-Length: ";

static const char HTTP_302_REDIRECT[] =
    "HTTP/1.1 302 Found\r\n"
    "Location: http://192.168.4.1/\r\n"
    "Connection: close\r\n"
    "\r\n";

static const char HTTP_400_BAD_REQUEST[] =
    "HTTP/1.1 400 Bad Request\r\n"
    "Connection: close\r\n"
    "\r\n";

// URL decode helper
static void url_decode(char* dst, const char* src, size_t dst_size)
{
    size_t di = 0;
    for (size_t si = 0; src[si] && di < dst_size - 1; si++)
    {
        if (src[si] == '%' && src[si + 1] && src[si + 2])
        {
            char hex[3] = {src[si + 1], src[si + 2], 0};
            dst[di++] = (char)strtol(hex, NULL, 16);
            si += 2;
        }
        else if (src[si] == '+')
        {
            dst[di++] = ' ';
        }
        else
        {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
}

// Parse form data and save configuration
static bool handle_configure_post(const char* body)
{
    char ssid[CONFIG_SSID_MAX_LEN + 1] = {0};
    char password[CONFIG_PASSWORD_MAX_LEN + 1] = {0};
    char rfs_ip[CONFIG_RFS_IP_MAX_LEN + 1] = {0};

    // Parse URL-encoded form data
    const char* p = body;
    while (*p)
    {
        // Find key
        const char* key_start = p;
        while (*p && *p != '=')
            p++;
        if (!*p)
            break;

        size_t key_len = p - key_start;
        p++; // Skip '='

        // Find value
        const char* val_start = p;
        while (*p && *p != '&')
            p++;

        size_t val_len = p - val_start;
        if (*p)
            p++; // Skip '&'

        // Extract value
        char val_encoded[128];
        if (val_len >= sizeof(val_encoded))
            val_len = sizeof(val_encoded) - 1;
        memcpy(val_encoded, val_start, val_len);
        val_encoded[val_len] = '\0';

        // Match keys and decode values
        if (key_len == 4 && strncmp(key_start, "ssid", 4) == 0)
        {
            url_decode(ssid, val_encoded, sizeof(ssid));
        }
        else if (key_len == 8 && strncmp(key_start, "password", 8) == 0)
        {
            url_decode(password, val_encoded, sizeof(password));
        }
        else if (key_len == 6 && strncmp(key_start, "rfs_ip", 6) == 0)
        {
            url_decode(rfs_ip, val_encoded, sizeof(rfs_ip));
        }
    }

    printf("[Captive] Received config: SSID='%s', RFS_IP='%s'\n", ssid, rfs_ip);

    // Validate
    if (ssid[0] == '\0')
    {
        printf("[Captive] Error: SSID is empty\n");
        return false;
    }

    // Save configuration
    bool saved = config_save(ssid, password, rfs_ip[0] ? rfs_ip : NULL);
    if (saved)
    {
        printf("[Captive] Configuration saved successfully\n");
    }

    return saved;
}

static err_t http_close_conn(http_conn_t* conn)
{
    if (!conn)
        return ERR_OK;

    struct tcp_pcb* pcb = conn->pcb;
    free(conn);

    if (pcb)
    {
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_err(pcb, NULL);
        tcp_close(pcb);
    }

    return ERR_OK;
}

static err_t http_send_response(http_conn_t* conn, const char* headers, const uint8_t* body, size_t body_len)
{
    struct tcp_pcb* pcb = conn->pcb;

    // Send headers
    tcp_write(pcb, headers, strlen(headers), TCP_WRITE_FLAG_COPY);

    // Send body if present
    if (body && body_len > 0)
    {
        tcp_write(pcb, body, body_len, TCP_WRITE_FLAG_COPY);
    }

    tcp_output(pcb);
    return ERR_OK;
}

static size_t build_device_info_json(char* out, size_t out_len)
{
    pico_unique_board_id_t board_id;
    pico_get_unique_board_id(&board_id);

    char id_hex[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];
    for (size_t i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; ++i)
    {
        snprintf(&id_hex[i * 2], 3, "%02x", board_id.id[i]);
    }
    id_hex[sizeof(id_hex) - 1] = '\0';

    const char* mdns_prefix = "altair-8800-";
    char mdns[32];
    snprintf(mdns, sizeof(mdns), "%s%02x%02x%02x%02x.local", mdns_prefix,
             board_id.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - 4],
             board_id.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - 3],
             board_id.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - 2],
             board_id.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - 1]);

    return (size_t)snprintf(out, out_len,
                            "{\"id\":\"%s\",\"mdns\":\"%s\"}",
                            id_hex, mdns);
}

static void http_process_request(http_conn_t* conn)
{
    char* req = conn->recv_buf;

    // Parse method and path
    bool is_get = strncmp(req, "GET ", 4) == 0;
    bool is_post = strncmp(req, "POST ", 5) == 0;

    if (!is_get && !is_post)
    {
        http_send_response(conn, HTTP_400_BAD_REQUEST, NULL, 0);
        http_close_conn(conn);
        return;
    }

    // Find body separator BEFORE modifying the buffer
    char* body_sep = strstr(req, "\r\n\r\n");
    char* body = body_sep ? body_sep + 4 : NULL;

    // Find path
    char* path_start = strchr(req, ' ') + 1;
    char* path_end = strchr(path_start, ' ');
    if (!path_end)
    {
        http_send_response(conn, HTTP_400_BAD_REQUEST, NULL, 0);
        http_close_conn(conn);
        return;
    }
    *path_end = '\0';

    printf("[Captive] HTTP %s %s\n", is_get ? "GET" : "POST", path_start);

    if (is_get && (strcmp(path_start, "/") == 0 || strcmp(path_start, "/index.html") == 0))
    {
        // Serve config page
        char header[256];
        snprintf(header, sizeof(header), "%s%zu\r\n\r\n", HTTP_200_HTML_GZ, config_page_gz_len);
        http_send_response(conn, header, config_page_gz, config_page_gz_len);
    }
    else if (is_get && strcmp(path_start, "/device.json") == 0)
    {
        char json_body[128];
        size_t json_len = build_device_info_json(json_body, sizeof(json_body));

        char header[256];
        snprintf(header, sizeof(header), "%s%zu\r\n\r\n", HTTP_200_JSON, json_len);
        http_send_response(conn, header, (const uint8_t*)json_body, json_len);
    }
    else if (is_post && strcmp(path_start, "/configure") == 0)
    {
        if (body)
        {
            if (handle_configure_post(body))
            {
                char json_body[128];
                size_t json_len = build_device_info_json(json_body, sizeof(json_body));

                char header[256];
                snprintf(header, sizeof(header), "%s%zu\r\n\r\n", HTTP_200_JSON, json_len);
                http_send_response(conn, header, (const uint8_t*)json_body, json_len);

                // Schedule reboot after a short delay (let response flush)
                printf("[Captive] Configuration saved, rebooting in 2 seconds...\n");
                reboot_pending = true;
                reboot_deadline = make_timeout_time_ms(2000);
            }
            else
            {
                http_send_response(conn, HTTP_400_BAD_REQUEST, NULL, 0);
            }
        }
        else
        {
            http_send_response(conn, HTTP_400_BAD_REQUEST, NULL, 0);
        }
    }
    else
    {
        // Redirect all other requests to root (captive portal behavior)
        http_send_response(conn, HTTP_302_REDIRECT, NULL, 0);
    }

    http_close_conn(conn);
}

static err_t http_recv_cb(void* arg, struct tcp_pcb* pcb, struct pbuf* p, err_t err)
{
    http_conn_t* conn = (http_conn_t*)arg;

    if (!p || err != ERR_OK)
    {
        // Connection closed
        http_close_conn(conn);
        return ERR_OK;
    }

    // Copy data to buffer
    size_t copy_len = p->tot_len;
    if (conn->recv_len + copy_len > HTTP_RECV_BUF_SIZE - 1)
    {
        copy_len = HTTP_RECV_BUF_SIZE - 1 - conn->recv_len;
    }

    pbuf_copy_partial(p, conn->recv_buf + conn->recv_len, copy_len, 0);
    conn->recv_len += copy_len;
    conn->recv_buf[conn->recv_len] = '\0';

    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);

    // Check if we have complete headers
    char* header_end = strstr(conn->recv_buf, "\r\n\r\n");
    if (header_end)
    {
        // For POST requests, check Content-Length and wait for body
        if (strncmp(conn->recv_buf, "POST ", 5) == 0)
        {
            // Find Content-Length header (check common casings)
            char* cl_header = strstr(conn->recv_buf, "Content-Length:");
            if (!cl_header)
                cl_header = strstr(conn->recv_buf, "content-length:");
            if (cl_header)
            {
                int content_length = atoi(cl_header + 15);
                char* body_start = header_end + 4;
                size_t header_size = body_start - conn->recv_buf;
                size_t body_received = conn->recv_len - header_size;
                
                // Wait until we have the complete body
                if ((int)body_received < content_length)
                {
                    return ERR_OK;  // Wait for more data
                }
            }
        }
        
        http_process_request(conn);
    }

    return ERR_OK;
}

static void http_err_cb(void* arg, err_t err)
{
    http_conn_t* conn = (http_conn_t*)arg;
    (void)err;

    if (conn)
    {
        conn->pcb = NULL; // PCB already freed by lwIP
        free(conn);
    }
}

static err_t http_accept_cb(void* arg, struct tcp_pcb* newpcb, err_t err)
{
    (void)arg;

    if (err != ERR_OK || !newpcb)
    {
        return ERR_VAL;
    }

    http_conn_t* conn = calloc(1, sizeof(http_conn_t));
    if (!conn)
    {
        tcp_abort(newpcb);
        return ERR_MEM;
    }

    conn->pcb = newpcb;

    tcp_arg(newpcb, conn);
    tcp_recv(newpcb, http_recv_cb);
    tcp_err(newpcb, http_err_cb);

    return ERR_OK;
}

static bool http_server_start(void)
{
    http_pcb = tcp_new();
    if (!http_pcb)
    {
        printf("[Captive] Failed to create HTTP PCB\n");
        return false;
    }

    err_t err = tcp_bind(http_pcb, IP_ADDR_ANY, CAPTIVE_PORTAL_HTTP_PORT);
    if (err != ERR_OK)
    {
        printf("[Captive] Failed to bind HTTP server: %d\n", err);
        tcp_close(http_pcb);
        http_pcb = NULL;
        return false;
    }

    http_pcb = tcp_listen(http_pcb);
    if (!http_pcb)
    {
        printf("[Captive] Failed to listen on HTTP server\n");
        return false;
    }

    tcp_accept(http_pcb, http_accept_cb);
    printf("[Captive] HTTP server started on port %d\n", CAPTIVE_PORTAL_HTTP_PORT);
    return true;
}

static void http_server_stop(void)
{
    if (http_pcb)
    {
        tcp_close(http_pcb);
        http_pcb = NULL;
    }
}

// ============================================================================
// Main Captive Portal API
// ============================================================================

bool captive_portal_start(void)
{
    if (portal_running)
    {
        return true;
    }

    printf("[Captive] Starting captive portal in AP mode...\n");

    // Enable AP mode
    cyw43_arch_enable_ap_mode(CAPTIVE_PORTAL_AP_SSID, NULL, CYW43_AUTH_OPEN);

    // Configure static IP for AP interface
    struct netif* netif = &cyw43_state.netif[CYW43_ITF_AP];
    ip4_addr_t ip, netmask, gw;
    IP4_ADDR(&ip, ap_ip[0], ap_ip[1], ap_ip[2], ap_ip[3]);
    IP4_ADDR(&netmask, ap_netmask[0], ap_netmask[1], ap_netmask[2], ap_netmask[3]);
    IP4_ADDR(&gw, ap_ip[0], ap_ip[1], ap_ip[2], ap_ip[3]);
    netif_set_addr(netif, &ip, &netmask, &gw);

    printf("[Captive] AP mode enabled: SSID='%s', IP=%s\n", CAPTIVE_PORTAL_AP_SSID, CAPTIVE_PORTAL_IP_ADDR);

    // Start DHCP server
    if (!dhcp_server_start())
    {
        printf("[Captive] Failed to start DHCP server\n");
        cyw43_arch_disable_ap_mode();
        return false;
    }

    // Start DNS server (for captive portal redirect)
    if (!dns_server_start())
    {
        printf("[Captive] Failed to start DNS server\n");
        dhcp_server_stop();
        cyw43_arch_disable_ap_mode();
        return false;
    }

    // Start HTTP server
    if (!http_server_start())
    {
        printf("[Captive] Failed to start HTTP server\n");
        dns_server_stop();
        dhcp_server_stop();
        cyw43_arch_disable_ap_mode();
        return false;
    }

    portal_running = true;
    printf("[Captive] Captive portal running. Connect to '%s' and open http://%s/\n", CAPTIVE_PORTAL_AP_SSID,
           CAPTIVE_PORTAL_IP_ADDR);

    return true;
}

void captive_portal_stop(void)
{
    if (!portal_running)
    {
        return;
    }

    printf("[Captive] Stopping captive portal...\n");

    http_server_stop();
    dns_server_stop();
    dhcp_server_stop();
    cyw43_arch_disable_ap_mode();

    portal_running = false;
    printf("[Captive] Captive portal stopped\n");
}

bool captive_portal_is_running(void)
{
    return portal_running;
}

void captive_portal_poll(void)
{
    if (reboot_pending && time_reached(reboot_deadline))
    {
        reboot_pending = false;
        watchdog_enable(1, 1);
        while (1)
        {
            tight_loop_contents();
        }
    }
}

const char* captive_portal_get_ip(void)
{
    return CAPTIVE_PORTAL_IP_ADDR;
}
