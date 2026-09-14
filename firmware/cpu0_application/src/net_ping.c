#include "net_ping.h"
#include "net_lwip.h"

#include "lwip/raw.h"
#include "lwip/prot/icmp.h"
#include "lwip/prot/ip.h"
#include "lwip/prot/ip4.h"
#include "lwip/inet_chksum.h"
#include "lwip/ip4_addr.h"
#include "lwip/dns.h"

#include "xil_printf.h"

#define PING_ID          0xAFAFU
#define PING_DATA_SIZE   32U
#define PING_TIMEOUT_MS  2000U
#define PING_INTERVAL_MS 1000U
#define DNS_TIMEOUT_MS   5000U

static struct raw_pcb *ping_pcb = NULL;
static volatile u16_t ping_recv_seqno;
static volatile u8_t ping_recv_pending;

/* DNS lookup state. dns_gethostbyname()'s callback only fires from
 * net_poll() (it runs on lwIP's normal call path, not an ISR), so
 * resolve_target() below pumps net_poll() itself while it waits, same
 * as ping's own reply wait loop does. */
static volatile u8_t dns_query_done;
static volatile u8_t dns_query_ok;
static ip_addr_t dns_query_addr;

static void dns_found_cb(const char *name, const ip_addr_t *ipaddr, void *arg)
{
    (void)name;
    (void)arg;

    if (ipaddr != NULL) {
        dns_query_addr = *ipaddr;
        dns_query_ok = 1U;
    }
    dns_query_done = 1U;
}

/* Accepts either a dotted-decimal IPv4 literal or a hostname to resolve
 * via DNS (server address comes from DHCP, see dhcp.c's LWIP_DNS handling
 * in net_lwip.c's dhcp_start()). Blocking on the hostname path -- pumps
 * net_poll() internally, same contract as the rest of this file. */
static int resolve_target(const char *target_str, ip_addr_t *out)
{
    err_t err;
    u32_t start_ms;

    if (ip4addr_aton(target_str, out) != 0) {
        return 1;
    }

    dns_query_done = 0U;
    dns_query_ok = 0U;
    err = dns_gethostbyname(target_str, &dns_query_addr, dns_found_cb, NULL);
    if (err == ERR_OK) {
        /* Already cached -- callback never fires for this case. */
        *out = dns_query_addr;
        return 1;
    }
    if (err != ERR_INPROGRESS) {
        xil_printf("PING: '%s' is not a valid IPv4 address and DNS lookup "
                   "could not start (err=%d)\r\n", target_str, (int)err);
        return 0;
    }

    xil_printf("PING: resolving '%s' via DNS...\r\n", target_str);
    start_ms = net_millis();
    while (!dns_query_done) {
        net_poll();
        if ((net_millis() - start_ms) >= DNS_TIMEOUT_MS) {
            xil_printf("PING: DNS lookup for '%s' timed out\r\n", target_str);
            return 0;
        }
    }
    if (!dns_query_ok) {
        xil_printf("PING: DNS lookup for '%s' failed (host not found or "
                   "no DNS server)\r\n", target_str);
        return 0;
    }

    *out = dns_query_addr;
    xil_printf("PING: '%s' resolved to %s\r\n", target_str,
               ip4addr_ntoa(out));
    return 1;
}

static u8_t ping_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p,
                       const ip_addr_t *addr)
{
    struct icmp_echo_hdr *iecho;

    (void)arg;
    (void)pcb;
    (void)addr;

    if (p->tot_len < (PBUF_IP_HLEN + sizeof(struct icmp_echo_hdr))) {
        return 0U; /* too short to be our echo reply; let lwIP keep looking */
    }

    iecho = (struct icmp_echo_hdr *)((u8_t *)p->payload + PBUF_IP_HLEN);
    if ((iecho->type == ICMP_ER) && (lwip_ntohs(iecho->id) == PING_ID)) {
        ping_recv_seqno = lwip_ntohs(iecho->seqno);
        ping_recv_pending = 1U;
        pbuf_free(p);
        return 1U; /* consumed */
    }
    return 0U;
}

void net_ping_init(void)
{
    ping_pcb = raw_new(IP_PROTO_ICMP);
    if (ping_pcb == NULL) {
        xil_printf("PING: raw_new failed\r\n");
        return;
    }
    raw_recv(ping_pcb, ping_recv, NULL);
    raw_bind(ping_pcb, IP_ADDR_ANY);
}

static err_t ping_send(const ip_addr_t *target, u16_t seqno)
{
    struct pbuf *p;
    struct icmp_echo_hdr *iecho;
    const u16_t data_len = (u16_t)(sizeof(struct icmp_echo_hdr) + PING_DATA_SIZE);
    err_t err;
    u16_t i;

    p = pbuf_alloc(PBUF_IP, data_len, PBUF_RAM);
    if (p == NULL) {
        return ERR_MEM;
    }

    iecho = (struct icmp_echo_hdr *)p->payload;
    ICMPH_TYPE_SET(iecho, ICMP_ECHO);
    ICMPH_CODE_SET(iecho, 0);
    iecho->chksum = 0U;
    iecho->id = lwip_htons(PING_ID);
    iecho->seqno = lwip_htons(seqno);

    for (i = 0U; i < PING_DATA_SIZE; i++) {
        ((u8_t *)(iecho + 1))[i] = (u8_t)(i & 0xFFU);
    }

    iecho->chksum = inet_chksum(iecho, data_len);

    err = raw_sendto(ping_pcb, p, target);
    pbuf_free(p);
    return err;
}

int net_ping_host(const char *target_str, int count)
{
    ip_addr_t target;
    int replies = 0;
    int seq;

    if (ping_pcb == NULL) {
        xil_printf("PING: not initialized\r\n");
        return 0;
    }
    if (net_get_netif() == NULL) {
        xil_printf("PING: network not initialized\r\n");
        return 0;
    }
    if (!resolve_target(target_str, &target)) {
        return 0;
    }
    if (ip4_addr_isany(netif_ip4_addr(net_get_netif()))) {
        xil_printf("PING: WARNING no IP address yet (DHCP not bound) -- sending anyway\r\n");
    }

    xil_printf("PING: %s, %d probe(s)\r\n", target_str, count);

    for (seq = 1; seq <= count; seq++) {
        u32_t start_ms;
        u32_t elapsed_ms;

        ping_recv_pending = 0U;
        if (ping_send(&target, (u16_t)seq) != ERR_OK) {
            xil_printf("PING: seq=%d send failed\r\n", seq);
            net_poll();
            continue;
        }

        start_ms = net_millis();
        for (;;) {
            net_poll();
            if (ping_recv_pending && (ping_recv_seqno == (u16_t)seq)) {
                elapsed_ms = net_millis() - start_ms;
                xil_printf("PING: seq=%d reply from %s, time=%lu ms\r\n",
                           seq, target_str, (unsigned long)elapsed_ms);
                replies++;
                break;
            }
            elapsed_ms = net_millis() - start_ms;
            if (elapsed_ms >= PING_TIMEOUT_MS) {
                xil_printf("PING: seq=%d timeout\r\n", seq);
                break;
            }
        }

        if (seq < count) {
            u32_t wait_start = net_millis();
            while ((net_millis() - wait_start) < PING_INTERVAL_MS) {
                net_poll();
            }
        }
    }

    xil_printf("PING: %d/%d replies received\r\n", replies, count);
    return replies;
}
