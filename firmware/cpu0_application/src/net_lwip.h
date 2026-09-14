#ifndef NET_LWIP_H
#define NET_LWIP_H

#include "lwip/netif.h"
#include "lwip/arch.h"

/* Bring up GEM0 + lwIP (RAW API, DHCP). Call once, after ethernet_test()
 * has already confirmed PHY link UP -- this only sets up the software
 * stack, it assumes the MAC/PHY hardware path already works. */
void net_init(void);

/* Call every main-loop tick: drains the GEM0 RX queue and services lwIP's
 * periodic timers (TCP/DHCP). Non-blocking. */
void net_poll(void);

/* Prints current IP/mask/gateway (0.0.0.0 while DHCP has not bound yet)
 * and link status to the UART console. */
void net_print_status(void);

/* NULL until net_init() has run. */
struct netif *net_get_netif(void);

/* Free-running millisecond counter driven by net_init()'s periodic timer
 * (NET_TIMER_PERIOD_MS resolution). 0 if net_init() has not run. */
u32_t net_millis(void);

#endif /* NET_LWIP_H */
