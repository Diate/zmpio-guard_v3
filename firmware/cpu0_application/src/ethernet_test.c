#include "ethernet_test.h"

#include "xparameters.h"
#include "xemacps.h"
#include "xemacps_hw.h"
#include "xstatus.h"
#include "xil_printf.h"
#include "xil_io.h"
#include "sleep.h"

/*
 * ============================================================================
 * Z7-Lite R11 / RTL8201F
 * ============================================================================
 *
 * Hardware:
 *
 *   Zynq PS GEM0
 *        |
 *       EMIO
 *        |
 *      MDIO
 *        |
 *   RTL8201F PHY
 *
 * PHY address from board schematic:
 *
 *   PHY_ADDR = 1
 *
 * This file currently tests ONLY:
 *
 *   1. GEM0 initialization
 *   2. MDIO/MDC configuration
 *   3. PHY register access
 *   4. PHY link status
 *
 * It does NOT start DMA or lwIP yet.
 *
 * ============================================================================
 */

#define ETH_PHY_ADDR       0U  /* MDIO scan confirms PHY ID 0x001CC816 at addr 0, not 1 as the schematic strap implies */

/* ============================================================================
 * Standard IEEE 802.3 PHY registers
 * ========================================================================== */

#define PHY_REG_BMCR       0x00U
#define PHY_REG_BMSR       0x01U
#define PHY_REG_PHYID1     0x02U
#define PHY_REG_PHYID2     0x03U

/* ============================================================================
 * BMCR bits
 * ========================================================================== */

#define BMCR_RESET         (1U << 15)
#define BMCR_LOOPBACK      (1U << 14)
#define BMCR_SPEED100      (1U << 13)
#define BMCR_AN_ENABLE     (1U << 12)
#define BMCR_POWER_DOWN    (1U << 11)
#define BMCR_RESTART_AN    (1U << 9)
#define BMCR_FULL_DUPLEX   (1U << 8)

/* ============================================================================
 * BMSR bits
 * ========================================================================== */

#define BMSR_LINK_STATUS   (1U << 2)
#define BMSR_AN_COMPLETE   (1U << 5)

/* ============================================================================
 * State
 * ========================================================================== */

static XEmacPs EthMac;
static int EthInitialized = 0;

/*
 * In your Vitis 2023.2 project the compiler command contains:
 *
 *     -DSDT
 *
 * Therefore this is the System Device Tree flow.
 *
 * In SDT flow XEmacPs_LookupConfig() uses the peripheral base address.
 *
 * In non-SDT flow it uses the device ID.
 */

#if defined(SDT)

#ifndef XPAR_XEMACPS_0_BASEADDR
#error "XPAR_XEMACPS_0_BASEADDR is not defined. Check xparameters.h / platform."
#endif

#else

#ifndef XPAR_XEMACPS_0_DEVICE_ID
#error "XPAR_XEMACPS_0_DEVICE_ID is not defined. Check xparameters.h / platform."
#endif

#endif

/*
 * Some PHYs (e.g. RTL8201F) need extra time after reset before the first
 * MDIO frame is accepted. Add a short delay after GEM init to let the
 * PHY finish its internal power-on sequence.
 */
#define PHY_POST_RESET_DELAY_MS  1000U

/* ============================================================================
 * Debug helper: print GEM Network Status Register
 * ========================================================================== */

static void print_mdio_status(void)
{
    u32 nwsr;
    u32 nwctrl;
    u32 nwcfg;
    UINTPTR mdio_base;

    /*
     * Use Config.BaseAddress for MDIO status check.
     * In full SDT flow, MDIO producer might be elsewhere,
     * but current driver doesn't expose that field.
     */
    mdio_base = EthMac.Config.BaseAddress;

    xil_printf(
        "ETH: MDIO base (from Config.BaseAddress) = 0x%08lx\r\n",
        (unsigned long)mdio_base
    );

    nwctrl = XEmacPs_ReadReg(mdio_base, XEMACPS_NWCTRL_OFFSET);
    nwcfg  = XEmacPs_ReadReg(mdio_base, XEMACPS_NWCFG_OFFSET);
    nwsr   = XEmacPs_ReadReg(mdio_base, XEMACPS_NWSR_OFFSET);

    xil_printf("ETH: NWCTRL = 0x%08lx\r\n", (unsigned long)nwctrl);
    xil_printf("ETH: NWCFG  = 0x%08lx\r\n", (unsigned long)nwcfg);
    xil_printf("ETH: NWSR   = 0x%08lx\r\n", (unsigned long)nwsr);

    if ((nwctrl & XEMACPS_NWCTRL_MDEN_MASK) != 0U) {
        xil_printf("ETH: MDIO port enabled\r\n");
    } else {
        xil_printf("ETH: MDIO port DISABLED\r\n");
    }

    if ((nwsr & XEMACPS_NWSR_MDIOIDLE_MASK) != 0U) {
        xil_printf("ETH: MDIO state = IDLE\r\n");
    } else {
        xil_printf("ETH: MDIO state = BUSY\r\n");
    }

    /*
     * XEMACPS_NWSR_MDIO_MASK reflects the MDIO input state.
     */
    xil_printf(
        "ETH: MDIO input state = %lu\r\n",
        (unsigned long)(
            (nwsr & XEMACPS_NWSR_MDIO_MASK) ?
            1U : 0U
        )
    );
}


/* ============================================================================
 * PHY scan: try all 32 MDIO addresses to find a responding PHY
 * ========================================================================== */

static int phy_scan_find_address(u32 *found_addr)
{
    u16 id1;
    u16 id2;
    LONG status;
    u32 addr;
    u32 nwsr;

    xil_printf("ETH: Scanning MDIO bus for PHY...\r\n");

    /*
     * Some PHYs (e.g. RTL8201F variants) respond at address 0 when no
     * strap resistors are populated. Scan 0-31.
     */
    for (addr = 0U; addr < 32U; addr++) {
        status = XEmacPs_PhyRead(&EthMac, addr, PHY_REG_PHYID1, &id1);
        nwsr = XEmacPs_ReadReg(EthMac.Config.BaseAddress, XEMACPS_NWSR_OFFSET);

        if (status != (LONG)XST_SUCCESS) {
            xil_printf(
                "ETH: addr=%lu read failed, NWSR=0x%08lx\r\n",
                (unsigned long)addr,
                (unsigned long)nwsr
            );
            continue;
        }
        status = XEmacPs_PhyRead(&EthMac, addr, PHY_REG_PHYID2, &id2);
        if (status != (LONG)XST_SUCCESS) {
            continue;
        }

        xil_printf(
            "ETH: addr=%lu id1=0x%04x id2=0x%04x\r\n",
            (unsigned long)addr,
            (unsigned int)id1,
            (unsigned int)id2
        );

        if ((id1 != 0x0000U) && (id1 != 0xFFFFU) &&
            (id2 != 0x0000U) && (id2 != 0xFFFFU)) {
            if (found_addr != NULL) {
                *found_addr = addr;
            }
            xil_printf("ETH: PHY found at address %lu\r\n", (unsigned long)addr);
            return XST_SUCCESS;
        }
    }

    xil_printf("ETH: No PHY found on MDIO bus\r\n");
    return XST_FAILURE;
}


/* ============================================================================
 * PHY read helper
 * ========================================================================== */

static int phy_read(u32 phy_addr, u32 reg, u16 *value)
{
    int status;

    if (value == NULL) {
        return XST_INVALID_PARAM;
    }

    status = XEmacPs_PhyRead(
        &EthMac,
        phy_addr,
        reg,
        value
    );

    if (status != XST_SUCCESS) {

        xil_printf(
            "ETH: MDIO read failed: "
            "phy=%lu reg=0x%02lx status=%d\r\n",
            (unsigned long)phy_addr,
            (unsigned long)reg,
            status
        );

        /*
         * Very useful when debugging XST_EMAC_MII_BUSY.
         */
        print_mdio_status();

        return status;
    }

    return XST_SUCCESS;
}


/* ============================================================================
 * PHY write helper
 * ========================================================================== */

static int phy_write(u32 phy_addr, u32 reg, u16 value)
{
    int status;

    status = XEmacPs_PhyWrite(
        &EthMac,
        phy_addr,
        reg,
        value
    );

    if (status != XST_SUCCESS) {

        xil_printf(
            "ETH: MDIO write failed: "
            "phy=%lu reg=0x%02lx value=0x%04x status=%d\r\n",
            (unsigned long)phy_addr,
            (unsigned long)reg,
            (unsigned int)value,
            status
        );

        print_mdio_status();

        return status;
    }

    return XST_SUCCESS;
}


/* ============================================================================
 * GEM0 initialization
 * ========================================================================== */

int ethernet_test_init(void)
{
    XEmacPs_Config *config;
    int status;

    xil_printf("\r\n");
    xil_printf("========================================\r\n");
    xil_printf("ETHERNET TEST INIT\r\n");
    xil_printf("========================================\r\n");

#if defined(SDT)

    xil_printf(
        "ETH: SDT build flow detected\r\n"
    );

    xil_printf(
        "ETH: GEM0 base from xparameters = 0x%08lx\r\n",
        (unsigned long)XPAR_XEMACPS_0_BASEADDR
    );

    /*
     * SDT:
     * LookupConfig() uses peripheral base address.
     */
    config = XEmacPs_LookupConfig(
        (UINTPTR)XPAR_XEMACPS_0_BASEADDR
    );

#else

    xil_printf(
        "ETH: Classic BSP build flow detected\r\n"
    );

    xil_printf(
        "ETH: GEM0 device ID = %d\r\n",
        XPAR_XEMACPS_0_DEVICE_ID
    );

    /*
     * Classic BSP:
     * LookupConfig() uses device ID.
     */
    config = XEmacPs_LookupConfig(
        XPAR_XEMACPS_0_DEVICE_ID
    );

#endif

    if (config == NULL) {

        xil_printf(
            "ETH: ERROR - XEmacPs_LookupConfig failed\r\n"
        );

        return XST_FAILURE;
    }

    xil_printf(
        "ETH: GEM base address = 0x%08lx\r\n",
        (unsigned long)config->BaseAddress
    );

    /*
     * ------------------------------------------------------------------------
     * Initialize the GEM driver.
     * ------------------------------------------------------------------------
     */

    status = XEmacPs_CfgInitialize(
        &EthMac,
        config,
        config->BaseAddress
    );

    if (status != XST_SUCCESS) {

        xil_printf(
            "ETH: ERROR - XEmacPs_CfgInitialize failed: %d\r\n",
            status
        );

        return status;
    }

    xil_printf(
        "ETH: GEM0 initialization PASS\r\n"
    );

    xil_printf(
        "ETH: Config.BaseAddress = 0x%08lx\r\n",
        (unsigned long)EthMac.Config.BaseAddress
    );

    /*
     * NOTE: In full SDT flow, MDIO producer might be at a different base.
     * Current driver version does not expose MdioProducerBaseAddr field.
     * For now, checking MDIO status from Config.BaseAddress.
     *
     * -----------------------------------------------------------------------
     * Configure MDC / MDIO clock.
     *
     * XEmacPs_PhyRead() documentation explicitly requires the MDIO clock
     * divisor to be configured before PHY access.
     *
     * Current project:
     *
     *   CPU peripheral clock ~= 100 MHz
     *
     * With MDC_DIV_64, the resulting MDC clock is safely below the
     * 2.5 MHz IEEE MII management clock limit.
     *
     * ------------------------------------------------------------------------
     */

    XEmacPs_SetMdioDivisor(
        &EthMac,
        MDC_DIV_224
    );

    xil_printf(
        "ETH: MDIO divisor configured = MDC_DIV_224\r\n"
    );

    /*
     * Print the MDIO state immediately after configuration.
     */
    print_mdio_status();

    /*
     * Wait for PHY to complete its power-on reset sequence.
     */
    xil_printf("ETH: Waiting %lu ms for PHY reset sequence...\r\n",
               (unsigned long)PHY_POST_RESET_DELAY_MS);
    sleep(PHY_POST_RESET_DELAY_MS / 1000U);

    /*
     * Some PHYs come up at address 0; do a quick explicit read there so
     * the user can see the value even if it is not used later.
     */
    {
        u16 bmcr_addr0 = 0;
        u16 bmsr_addr0 = 0;
        LONG status0;

        status0 = XEmacPs_PhyRead(&EthMac, 0U, PHY_REG_BMCR, &bmcr_addr0);
        if (status0 == (LONG)XST_SUCCESS) {
            (void)XEmacPs_PhyRead(&EthMac, 0U, PHY_REG_BMSR, &bmsr_addr0);
            xil_printf("ETH: address 0 BMCR=0x%04x BMSR=0x%04x\r\n",
                       (unsigned int)bmcr_addr0, (unsigned int)bmsr_addr0);
        }
    }

    EthInitialized = 1;

    return XST_SUCCESS;
}


/* ============================================================================
 * PHY ID / register test
 * ========================================================================== */

int ethernet_test_phy(void)
{
    u16 phy_id1 = 0;
    u16 phy_id2 = 0;
    u16 bmcr = 0;
    u16 bmsr = 0;

    int status;

    if (!EthInitialized) {

        xil_printf(
            "ETH: ERROR - Ethernet not initialized\r\n"
        );

        return XST_FAILURE;
    }

    xil_printf("\r\n");
    xil_printf("----------------------------------------\r\n");
    xil_printf("ETHERNET PHY / MDIO TEST\r\n");
    xil_printf("----------------------------------------\r\n");

    xil_printf(
        "ETH: PHY address = %lu\r\n",
        (unsigned long)ETH_PHY_ADDR
    );

/*
 * The RTL8201F may not be at address 1 on all boards. Scan first.
 */
{
    u32 detected_addr = ETH_PHY_ADDR;
    int scan_status;

    scan_status = phy_scan_find_address(&detected_addr);
    if (scan_status == XST_SUCCESS) {
        if (detected_addr != ETH_PHY_ADDR) {
            xil_printf(
                "ETH: WARNING - PHY found at %lu, expected %lu; "
                "update ETH_PHY_ADDR for this board\r\n",
                (unsigned long)detected_addr,
                (unsigned long)ETH_PHY_ADDR
            );
        }
    } else {
        xil_printf(
            "ETH: WARNING - PHY scan failed, continuing with addr %lu\r\n",
            (unsigned long)ETH_PHY_ADDR
        );
    }
}

    /*
     * ------------------------------------------------------------------------
     * PHY ID1
     * ------------------------------------------------------------------------
     */

    status = phy_read(
        ETH_PHY_ADDR,
        PHY_REG_PHYID1,
        &phy_id1
    );

    if (status != XST_SUCCESS) {
        return status;
    }

    /*
     * ------------------------------------------------------------------------
     * PHY ID2
     * ------------------------------------------------------------------------
     */

    status = phy_read(
        ETH_PHY_ADDR,
        PHY_REG_PHYID2,
        &phy_id2
    );

    if (status != XST_SUCCESS) {
        return status;
    }

    xil_printf(
        "ETH: PHY ID1 = 0x%04x\r\n",
        (unsigned int)phy_id1
    );

    xil_printf(
        "ETH: PHY ID2 = 0x%04x\r\n",
        (unsigned int)phy_id2
    );

    /*
     * 0x0000 / 0xFFFF are treated as invalid for this initial bring-up.
     */

    if ((phy_id1 == 0x0000U) ||
        (phy_id1 == 0xFFFFU) ||
        (phy_id2 == 0x0000U) ||
        (phy_id2 == 0xFFFFU)) {

        xil_printf(
            "ETH: FAIL - invalid PHY ID response; PHY may be in reset or wrong address\r\n"
        );

        return XST_FAILURE;
    }

    xil_printf(
        "ETH: PHY ID read PASS\r\n"
    );

    /*
     * ------------------------------------------------------------------------
     * BMCR
     * ------------------------------------------------------------------------
     */

    status = phy_read(
        ETH_PHY_ADDR,
        PHY_REG_BMCR,
        &bmcr
    );

    if (status != XST_SUCCESS) {
        return status;
    }

    xil_printf(
        "ETH: BMCR = 0x%04x\r\n",
        (unsigned int)bmcr
    );

    /*
     * ------------------------------------------------------------------------
     * BMSR
     *
     * Read twice because the Link Status bit is commonly implemented
     * as latch-low behavior.
     * ------------------------------------------------------------------------
     */

    status = phy_read(
        ETH_PHY_ADDR,
        PHY_REG_BMSR,
        &bmsr
    );

    if (status != XST_SUCCESS) {
        return status;
    }

    status = phy_read(
        ETH_PHY_ADDR,
        PHY_REG_BMSR,
        &bmsr
    );

    if (status != XST_SUCCESS) {
        return status;
    }

    xil_printf(
        "ETH: BMSR = 0x%04x\r\n",
        (unsigned int)bmsr
    );

    xil_printf(
        "ETH: Link status      = %s\r\n",
        (bmsr & BMSR_LINK_STATUS) ?
            "UP" : "DOWN"
    );

    xil_printf(
        "ETH: Auto-negotiation = %s\r\n",
        (bmsr & BMSR_AN_COMPLETE) ?
            "COMPLETE" :
            "NOT COMPLETE"
    );

    return XST_SUCCESS;
}


/* ============================================================================
 * Link test
 * ========================================================================== */

int ethernet_test_link(void)
{
    u16 bmsr;
    int status;
    int i;

    if (!EthInitialized) {

        xil_printf(
            "ETH: ERROR - Ethernet not initialized\r\n"
        );

        return XST_FAILURE;
    }

    xil_printf("\r\n");
    xil_printf("----------------------------------------\r\n");
    xil_printf("ETHERNET LINK TEST\r\n");
    xil_printf("----------------------------------------\r\n");

    xil_printf(
        "ETH: waiting for PHY link"
    );

    /*
     * Give PHY/link partner time to complete auto-negotiation.
     *
     * 20 seconds maximum in this simple bring-up test.
     */
    for (i = 0; i < 20; ++i) {

        /*
         * First read may clear a latch-low indication.
         */
        status = phy_read(
            ETH_PHY_ADDR,
            PHY_REG_BMSR,
            &bmsr
        );

        if (status != XST_SUCCESS) {

            xil_printf("\r\n");

            return status;
        }

        /*
         * Second read gives current practical link status.
         */
        status = phy_read(
            ETH_PHY_ADDR,
            PHY_REG_BMSR,
            &bmsr
        );

        if (status != XST_SUCCESS) {

            xil_printf("\r\n");

            return status;
        }

        if ((bmsr & BMSR_LINK_STATUS) != 0U) {

            xil_printf(" UP\r\n");

            xil_printf(
                "ETH: LINK UP\r\n"
            );

            return XST_SUCCESS;
        }

        xil_printf(".");

        sleep(1);
    }

    xil_printf(
        " TIMEOUT\r\n"
    );

    xil_printf(
        "ETH: LINK DOWN\r\n"
    );

    return XST_FAILURE;
}


/* ============================================================================
 * Complete Ethernet basic test
 * ========================================================================== */

int ethernet_test(void)
{
    int status;

    /*
     * 1. GEM initialization
     */
    status = ethernet_test_init();

    if (status != XST_SUCCESS) {

        xil_printf(
            "ETH: GEM initialization FAILED\r\n"
        );

        return status;
    }

    /*
     * 2. MDIO / PHY register access
     */
    status = ethernet_test_phy();

    if (status != XST_SUCCESS) {

        xil_printf(
            "ETH: PHY / MDIO test FAILED\r\n"
        );

        return status;
    }

    /*
     * 3. Link
     */
    status = ethernet_test_link();

    if (status != XST_SUCCESS) {

        xil_printf(
            "ETH: LINK test FAILED\r\n"
        );

        return status;
    }

    xil_printf("\r\n");
    xil_printf("========================================\r\n");
    xil_printf("ETHERNET BASIC TEST PASS\r\n");
    xil_printf("========================================\r\n");

    return XST_SUCCESS;
}