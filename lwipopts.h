#ifndef __LWIPOPTS_H__
#define __LWIPOPTS_H__

// Common settings used in most of the pico_w examples
// (see https://www.nongnu.org/lwip/2_1_x/group__lwip__opts.html for details)

// allow override in some examples
#ifndef NO_SYS
#define NO_SYS 1 // Use lwIP without OS-awareness (no threads, semaphores, mutexes)
#endif
// allow override in some examples
#ifndef LWIP_SOCKET
#define LWIP_SOCKET 0 // Disable BSD-style socket API
#endif
#define MEM_LIBC_MALLOC 0 // Use lwIP's internal memory pool
#define MEM_ALIGNMENT 4   // Memory alignment (4 bytes for ARM)
#if defined(PICO_RP2040)
#define MEM_SIZE 51200 // Size of the heap memory (bytes) - Increased to 64KB to stop allocation errors
#else
#define MEM_SIZE 65536 // Size of the heap memory (bytes) - Increased to 64KB for RP2350
#endif
#define MEMP_NUM_TCP_SEG 32       // Number of simultaneously queued TCP segments - Increased for larger responses
#define MEMP_NUM_ARP_QUEUE 10     // Number of packets queued waiting for ARP resolution
#define MEMP_NUM_TCP_PCB 16       // Number of simultaneously active TCP connections - Increased to handle TIME_WAIT
#define MEMP_NUM_TCP_PCB_LISTEN 5 // Number of listening TCP connections - Increased to prevent lockout
#define PBUF_POOL_SIZE 24         // Number of buffers in the pbuf pool - Reduced for memory savings
#define LWIP_ARP 1                // Enable ARP protocol
#define LWIP_ETHERNET 1           // Enable Ethernet support
#define LWIP_ICMP 1               // Enable ICMP protocol (ping)
#define LWIP_RAW 1                // Enable raw IP sockets
#define TCP_WND (6 * TCP_MSS)     // TCP receive window size - Balanced for 8KB HTML content
#define TCP_MSS 1460              // TCP maximum segment size (bytes)
#define TCP_SND_BUF (6 * TCP_MSS) // TCP sender buffer space (bytes) - Sized to accommodate HTML page
#define TCP_SND_QUEUELEN ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS)) // TCP sender buffer space (pbufs)
#define LWIP_NETIF_STATUS_CALLBACK 1                                       // Enable network interface status callbacks
#define LWIP_NETIF_LINK_CALLBACK 1                                         // Enable link status change callbacks
#define LWIP_NETIF_HOSTNAME 1                                              // Support hostname in DHCP requests
#define LWIP_NETCONN 0                                                     // Disable sequential API (netconn)
#define MEM_STATS 1                                                        // Enable memory statistics
#define SYS_STATS 1                                                        // Enable system statistics
#define MEMP_STATS 1                                                       // Enable memory pool statistics
#define LINK_STATS 1                                                       // Enable link layer statistics
// #define ETH_PAD_SIZE                2  // Ethernet padding for 32-bit alignment
#define LWIP_CHKSUM_ALGORITHM 3     // Checksum algorithm (3 = optimized)
#define LWIP_DHCP 1                 // Enable DHCP client
#define LWIP_IPV4 1                 // Enable IPv4 protocol
#define LWIP_TCP 1                  // Enable TCP protocol
#define LWIP_UDP 1                  // Enable UDP protocol
#define LWIP_DNS 1                  // Enable DNS client
#define LWIP_HTTPC 1                // Enable HTTP client
#define LWIP_TCP_KEEPALIVE 1        // Enable TCP keepalive
#define LWIP_NETIF_TX_SINGLE_PBUF 1 // Put all data to send into one pbuf (for DMA compatibility)
#define DHCP_DOES_ARP_CHECK 0       // Disable ARP check on offered DHCP address
#define LWIP_DHCP_DOES_ACD_CHECK 0  // Disable Address Conflict Detection

#define LWIP_STATS 1 // Enable statistics collection (always enabled for monitoring)

#ifndef NDEBUG
#define LWIP_DEBUG 1         // Enable debug output in debug builds
#define LWIP_STATS_DISPLAY 1 // Enable statistics display in debug builds
#endif

#define ETHARP_DEBUG LWIP_DBG_OFF     // Disable Ethernet/ARP debug messages
#define NETIF_DEBUG LWIP_DBG_OFF      // Disable network interface debug messages
#define PBUF_DEBUG LWIP_DBG_OFF       // Disable packet buffer debug messages
#define API_LIB_DEBUG LWIP_DBG_OFF    // Disable API library debug messages
#define API_MSG_DEBUG LWIP_DBG_OFF    // Disable API message debug messages
#define SOCKETS_DEBUG LWIP_DBG_OFF    // Disable socket debug messages
#define ICMP_DEBUG LWIP_DBG_OFF       // Disable ICMP debug messages
#define INET_DEBUG LWIP_DBG_OFF       // Disable inet debug messages
#define IP_DEBUG LWIP_DBG_OFF         // Disable IP debug messages
#define IP_REASS_DEBUG LWIP_DBG_OFF   // Disable IP reassembly debug messages
#define RAW_DEBUG LWIP_DBG_OFF        // Disable raw IP debug messages
#define MEM_DEBUG LWIP_DBG_OFF        // Disable memory debug messages
#define MEMP_DEBUG LWIP_DBG_OFF       // Disable memory pool debug messages
#define SYS_DEBUG LWIP_DBG_OFF        // Disable system debug messages
#define TCP_DEBUG LWIP_DBG_OFF        // Disable TCP debug messages
#define TCP_INPUT_DEBUG LWIP_DBG_OFF  // Disable TCP input debug messages
#define TCP_OUTPUT_DEBUG LWIP_DBG_OFF // Disable TCP output debug messages
#define TCP_RTO_DEBUG LWIP_DBG_OFF    // Disable TCP retransmission timeout debug messages
#define TCP_CWND_DEBUG LWIP_DBG_OFF   // Disable TCP congestion window debug messages
#define TCP_WND_DEBUG LWIP_DBG_OFF    // Disable TCP window debug messages
#define TCP_FR_DEBUG LWIP_DBG_OFF     // Disable TCP fast retransmit debug messages
#define TCP_QLEN_DEBUG LWIP_DBG_OFF   // Disable TCP queue length debug messages
#define TCP_RST_DEBUG LWIP_DBG_OFF    // Disable TCP reset debug messages
#define UDP_DEBUG LWIP_DBG_OFF        // Disable UDP debug messages
#define TCPIP_DEBUG LWIP_DBG_OFF      // Disable TCP/IP thread debug messages
#define PPP_DEBUG LWIP_DBG_OFF        // Disable PPP debug messages
#define SLIP_DEBUG LWIP_DBG_OFF       // Disable SLIP debug messages
#define DHCP_DEBUG LWIP_DBG_OFF       // Disable DHCP debug messages

#endif /* __LWIPOPTS_H__ */
