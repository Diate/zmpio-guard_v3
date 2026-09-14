#include "net_lwip.h"

#include "lwip/init.h"
#include "lwip/dhcp.h"
#include "lwip/tcp.h"
#include "lwip/ip4_addr.h"
#include "netif/etharp.h"
#include "netif/xadapter.h"

#include "xil_printf.h"
#include "xparameters.h"
#include "xscugic.h"
#include "xscutimer.h"
#include "xstatus.h"
#include "xinterrupt_wrap.h"

/*
 * This Xilinx lwip213 port's RAW-API build (sys_arch_raw.c) does not
 * implement sys_now()/sys_check_timeouts() -- the app is expected to drive
 * lwIP's periodic timers itself. This mirrors the reference platform_zynq.c
 * pattern shipped with the library (examples/lwip_example_platform_zynq.c),
 * adapted to the SDT interrupt API (XSetupInterruptSystem) used by this
 * project instead of that reference's classic-flow XScuGic_DeviceInitialize.
 */
#define NET_TIMER_PERIOD_MS   50U
#define NET_DHCP_FINE_MS      500U
#define NET_DHCP_COARSE_MS    60000U
#define NET_ARP_TMR_MS        5000U

static struct netif server_netif;
static struct netif *active_netif = NULL;
static XScuTimer net_timer;

static volatile u32_t net_ms_ticks = 0U;
static volatile int TcpFastTmrFlag = 0;
static volatile int TcpSlowTmrFlag = 0;
static int net_dhcp_bound_logged = 0;

static void net_timer_callback(void *ref)
{
    XScuTimer *timer = (XScuTimer *)ref;
    static u32_t dhcp_fine_acc = 0U;
    static u32_t dhcp_coarse_acc = 0U;
    static u32_t arp_acc = 0U;
    static u32_t tcp_slow_acc = 0U;

    net_ms_ticks += NET_TIMER_PERIOD_MS;
    TcpFastTmrFlag = 1;

    tcp_slow_acc += NET_TIMER_PERIOD_MS;
    if (tcp_slow_acc >= 500U) {
        tcp_slow_acc = 0U;
        TcpSlowTmrFlag = 1;
    }

    arp_acc += NET_TIMER_PERIOD_MS;
    if (arp_acc >= NET_ARP_TMR_MS) {
        arp_acc = 0U;
        etharp_tmr();
    }

#if LWIP_DHCP
    dhcp_fine_acc += NET_TIMER_PERIOD_MS;
    if (dhcp_fine_acc >= NET_DHCP_FINE_MS) {
        dhcp_fine_acc = 0U;
        dhcp_fine_tmr();
    }
    dhcp_coarse_acc += NET_TIMER_PERIOD_MS;
    if (dhcp_coarse_acc >= NET_DHCP_COARSE_MS) {
        dhcp_coarse_acc = 0U;
        dhcp_coarse_tmr();
    }
#endif

    XScuTimer_ClearInterruptStatus(timer);
}

static int net_setup_timer(void)
{
    XScuTimer_Config *config = XScuTimer_LookupConfig(XPAR_XSCUTIMER_0_BASEADDR);

    if (config == NULL) {
        xil_printf("NET: XScuTimer_LookupConfig failed\r\n");
        return XST_FAILURE;
    }
    if (XScuTimer_CfgInitialize(&net_timer, config, config->BaseAddr) != XST_SUCCESS) {
        xil_printf("NET: XScuTimer_CfgInitialize failed\r\n");
        return XST_FAILURE;
    }

    XScuTimer_EnableAutoReload(&net_timer);
    /* SCU private timer clocks at CPU_CLK/2 (ARM Cortex-A9 MPCore TRM);
     * XPAR_CPU_CORE_CLOCK_FREQ_HZ/8 == (CPU_CLK/2) * 0.25s is the proven
     * 250 ms divisor used by Xilinx's own lwIP reference platform_zynq.c --
     * scale that same ratio down to NET_TIMER_PERIOD_MS. */
    XScuTimer_LoadTimer(&net_timer,
        (XPAR_CPU_CORE_CLOCK_FREQ_HZ / 8U) * NET_TIMER_PERIOD_MS / 250U);

    if (XSetupInterruptSystem(&net_timer, (void *)net_timer_callback,
            config->IntrId, config->IntrParent,
            XINTERRUPT_DEFAULT_PRIORITY) != XST_SUCCESS) {
        xil_printf("NET: XSetupInterruptSystem (timer) failed\r\n");
        return XST_FAILURE;
    }

    XScuTimer_EnableInterrupt(&net_timer);
    XScuTimer_Start(&net_timer);
    return XST_SUCCESS;
}

void net_init(void)
{
    ip_addr_t ipaddr, netmask, gw;
    unsigned char mac_ethernet_address[6] = {0x00, 0x0a, 0x35, 0x00, 0x01, 0x02};

    xil_printf("\r\n----------------------------------------\r\n");
    xil_printf("NET: lwIP init (DHCP)\r\n");
    xil_printf("----------------------------------------\r\n");

    IP4_ADDR(&ipaddr,  0, 0, 0, 0);
    IP4_ADDR(&netmask, 0, 0, 0, 0);
    IP4_ADDR(&gw,      0, 0, 0, 0);

    lwip_init();

    active_netif = xemac_add(&server_netif, &ipaddr, &netmask, &gw,
                              mac_ethernet_address,
                              XPAR_XEMACPS_0_BASEADDR);
    if (active_netif == NULL) {
        xil_printf("NET: xemac_add failed -- network stays down\r\n");
        return;
    }

    netif_set_default(&server_netif);

    if (net_setup_timer() != XST_SUCCESS) {
        xil_printf("NET: timer setup failed -- DHCP/ARP/TCP timers will not run\r\n");
    }

    netif_set_up(&server_netif);

#if LWIP_DHCP
    dhcp_start(&server_netif);
    xil_printf("NET: DHCP started; use 'n' to check lease status\r\n");
#else
    xil_printf("NET: LWIP_DHCP=0 in this build -- no IP will be assigned\r\n");
#endif
}

void net_poll(void)
{
    if (active_netif == NULL) {
        return;
    }

    xemacif_input(active_netif);

    if (TcpFastTmrFlag) {
        tcp_fasttmr();
        TcpFastTmrFlag = 0;
    }
    if (TcpSlowTmrFlag) {
        tcp_slowtmr();
        TcpSlowTmrFlag = 0;
    }

    if (!net_dhcp_bound_logged &&
        !ip4_addr_isany(netif_ip4_addr(active_netif))) {
        xil_printf("NET: DHCP bound, IP = %s\r\n",
                   ip4addr_ntoa(netif_ip4_addr(active_netif)));
        net_dhcp_bound_logged = 1;
    }
}

void net_print_status(void)
{
    if (active_netif == NULL) {
        xil_printf("NET: not initialized\r\n");
        return;
    }
    xil_printf("NET: IP      = %s\r\n", ip4addr_ntoa(netif_ip4_addr(active_netif)));
    xil_printf("NET: Netmask = %s\r\n", ip4addr_ntoa(netif_ip4_netmask(active_netif)));
    xil_printf("NET: Gateway = %s\r\n", ip4addr_ntoa(netif_ip4_gw(active_netif)));
    xil_printf("NET: Link    = %s\r\n", netif_is_link_up(active_netif) ? "UP" : "DOWN");
    xil_printf("NET: DHCP    = %s\r\n",
               ip4_addr_isany(netif_ip4_addr(active_netif)) ?
                   "waiting for lease..." : "bound");
}

struct netif *net_get_netif(void)
{
    return active_netif;
}

u32_t net_millis(void)
{
    return net_ms_ticks;
}
