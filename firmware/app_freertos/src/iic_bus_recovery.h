#ifndef IIC_BUS_RECOVERY_H
#define IIC_BUS_RECOVERY_H

#include "xil_types.h"

/*
 * Best-effort recovery for an AXI IIC bus wedged by an interrupted
 * transaction -- e.g. the sensor left holding SDA low across a JTAG
 * reload/reboot. XIic_Reset() alone does not fix this: it only resets the
 * master-side core state, and XIic_Send()/XIic_Recv() refuse to touch the
 * bus at all once the core's Bus Busy status is stuck (XIic_WaitBusFree()
 * just polls it for ~1 s and gives up without ever driving SCL).
 *
 * This forces several address-byte transmissions through the same AXI IIC
 * core anyway, bypassing that upfront busy-wait. The real SCL activity this
 * generates can clock a slave that is mid-byte into finally releasing SDA
 * -- the same mechanism a GPIO-based 9-clock bus recovery relies on, just
 * driven through the existing hardware instead of a spare pin.
 *
 * NOT guaranteed: if the sensor is well and truly latched up, only a real
 * power-on reset (physical unplug/replug, or a GPIO-controlled power
 * switch) reliably clears it. Every internal wait here is bounded, so a
 * failed attempt cannot hang the caller.
 *
 * Returns 1 if the bus reads idle (not busy) afterward, 0 otherwise.
 */
int iic_bus_force_recover(UINTPTR base_address, u8 address, unsigned attempts);

#endif
