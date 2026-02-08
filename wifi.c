#include "wifi.h"

#include "pico/cyw43_arch.h"
#include "pico/error.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"

#include "cyw43.h"

#include "lwip/ip4_addr.h"
#include "lwip/netif.h"

#include <stdio.h>
#include <string.h>

static bool wifi_hw_ready = false;
static bool wifi_connected = false;
static char wifi_ip_address[16] = {0}; // Cached IP address string
static char wifi_hostname[20] = {0};   // Cached hostname (e.g., "altair-ab12")

bool wifi_is_ready(void)
{
    return wifi_hw_ready;
}

bool wifi_is_connected(void)
{
    return wifi_connected;
}

void wifi_set_ready(bool ready)
{
    wifi_hw_ready = ready;
    printf("[WiFi] Hardware ready set to: %d\n", ready);
}

void wifi_set_connected(bool connected)
{
    wifi_connected = connected;
    printf("[WiFi] Connected set to: %d\n", connected);

    // Clear IP address when disconnected
    if (!connected)
    {
        wifi_ip_address[0] = '\0';
    }
}

void wifi_set_ip_address(const char* ip)
{
    if (ip && ip[0] != '\0')
    {
        strncpy(wifi_ip_address, ip, sizeof(wifi_ip_address) - 1);
        wifi_ip_address[sizeof(wifi_ip_address) - 1] = '\0';
        printf("[WiFi] IP address cached: %s\n", wifi_ip_address);
    }
}

const char* wifi_get_ip_address(void)
{
    return wifi_ip_address[0] != '\0' ? wifi_ip_address : NULL;
}

const char* wifi_get_hostname(void)
{
    // Generate hostname on first call (lazy initialization)
    if (wifi_hostname[0] == '\0')
    {
        pico_unique_board_id_t board_id;
        pico_get_unique_board_id(&board_id);
        snprintf(wifi_hostname, sizeof(wifi_hostname), "altair-%02x%02x",
                 board_id.id[6], board_id.id[7]);
    }
    return wifi_hostname;
}

bool wifi_get_ip(char* buffer, size_t length)
{
    printf("[WiFi] wifi_get_ip called: hw_ready=%d, connected=%d\n", wifi_hw_ready, wifi_connected);

    if (!wifi_hw_ready || !buffer || length == 0)
    {
        printf("[WiFi] wifi_get_ip early return: hw_ready=%d, buffer=%p, length=%zu\n", wifi_hw_ready, buffer, length);
        return false;
    }

    bool ok = false;

    cyw43_arch_lwip_begin();
    struct netif* netif = &cyw43_state.netif[CYW43_ITF_STA];
    if (netif && netif_is_up(netif))
    {
        const ip4_addr_t* addr = netif_ip4_addr(netif);
        if (addr)
        {
            ok = ip4addr_ntoa_r(addr, buffer, length) != NULL;
            if (ok)
            {
                printf("[WiFi] Got IP address: %s\n", buffer);
            }
        }
        else
        {
            printf("[WiFi] IP address is NULL\n");
        }
    }
    else
    {
        printf("[WiFi] netif not up: netif=%p, is_up=%d\n", netif, netif ? netif_is_up(netif) : 0);
    }
    cyw43_arch_lwip_end();

    return ok;
}
