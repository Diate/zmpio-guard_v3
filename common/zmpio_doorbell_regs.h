#ifndef ZMPIO_DOORBELL_REGS_H
#define ZMPIO_DOORBELL_REGS_H

/*
 * Register offsets for zmpio_doorbell.v (hardware/rtl/src/zmpio_doorbell.v),
 * the one-way CPU1 -> CPU0 wakeup doorbell.
 *
 * Shared by both sides' drivers -- pl_doorbell.{c,h} under app_freertos and
 * cpu0_application -- purely so the two cannot drift. This is a plain MMIO
 * register map: it is NOT part of the ABI v3 shared-DDR layout_hash contract
 * and carries no version of its own.
 *
 * The base address is peripheral-instance-specific, assigned by Vivado's
 * address editor when hardware/rtl/bd/add_zmpio_doorbell.tcl is applied, so it
 * is NOT defined here. Each side's pl_doorbell.h has its own *_BASEADDR macro,
 * filled in from firmware/platform_dual/hw/sdt/pl.dtsi after the block design
 * is rebuilt -- the same convention as FPGA_DSP_HAL_DSP_CTRL_BASEADDR.
 */
#define ZMPIO_DOORBELL_REG_STATUS      0x00U /* R:  bit0 PENDING */
#define ZMPIO_DOORBELL_REG_SET         0x04U /* W:  bit0=1 -> PENDING<=1, COUNT++ */
#define ZMPIO_DOORBELL_REG_ACK         0x08U /* W:  bit0=1 -> PENDING<=0 */
#define ZMPIO_DOORBELL_REG_IRQ_ENABLE  0x0CU /* R/W: bit0 */
#define ZMPIO_DOORBELL_REG_COUNT       0x10U /* R:  total DBELL_SET writes, saturating */

#define ZMPIO_DOORBELL_BIT_PENDING     0x1U
#define ZMPIO_DOORBELL_BIT_SET         0x1U
#define ZMPIO_DOORBELL_BIT_ACK         0x1U
#define ZMPIO_DOORBELL_BIT_IRQ_ENABLE  0x1U

#endif
