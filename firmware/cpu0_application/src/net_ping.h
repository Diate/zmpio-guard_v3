#ifndef NET_PING_H
#define NET_PING_H

/* Register the raw ICMP PCB used for ping. Call once, after net_init(). */
void net_ping_init(void);

/* Send `count` ICMP echo requests to `target` (dotted-decimal IPv4, e.g.
 * "192.168.1.1", or a hostname such as "google.com" -- resolved via DNS
 * first, which requires DHCP to have handed out a DNS server), ~1 s apart,
 * printing RTT or timeout for each to the UART console. Blocking -- pumps
 * net_poll() internally while waiting, so it only stalls the rest of
 * main()'s loop for the duration of the command.
 * Returns the number of replies received. */
int net_ping_host(const char *target, int count);

#endif /* NET_PING_H */
