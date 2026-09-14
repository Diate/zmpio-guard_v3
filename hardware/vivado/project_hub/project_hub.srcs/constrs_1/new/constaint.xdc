# =============================================================================
# FILE: constraint.xdc
# PROJECT: MicroPhase Z7-Lite R11 / XC7Z020-CLG400
#
# Current hardware:
#
#   AXI IIC      -> MPU6050
#   PS I2C0      -> EMIO
#   PS SPI0      -> EMIO -> microSD
#   PS UART0     -> MIO14/15 -> onboard USB-UART
#   PS UART1     -> EMIO -> JP2
#
#   PS GEM0
#      |
#     EMIO / GMII
#      |
#   gmii_to_mii (custom RTL)
#      |
#      MII
#      |
#   RTL8201F
#      |
#     RJ45
#
# Ethernet PHY:
#   RTL8201F
#   10/100 Mbps
#   MII interface
#
# =============================================================================
# ETH PIN MAPPING - Z7-Lite R11
# =============================================================================
#
# Ethernet:
#
#   ETH_MDC       = G14
#   ETH_MDIO      = J15
#   ETH_nRST      = H20
#
#   ETH_TXCK      = L14
#   ETH_TXCTL     = N16
#   ETH_TXD0      = M14
#   ETH_TXD1      = L15
#   ETH_TXD2      = M15
#   ETH_TXD3      = N15
#
#   ETH_RXCK      = K17
#   ETH_RXDV      = K18
#   ETH_RXD0      = J14
#   ETH_RXD1      = K14
#   ETH_RXD2      = M18
#   ETH_RXD3      = M17
#
# UART1:
#   TX = K19
#   RX = M19
#
# =============================================================================


# =============================================================================
# 1. MPU6050 - AXI IIC
# =============================================================================
#
# SCL = Y16
# SDA = Y17
#
# =============================================================================

set_property PACKAGE_PIN Y16 [get_ports IIC_0_scl_io]
set_property IOSTANDARD LVCMOS33 [get_ports IIC_0_scl_io]
set_property PULLTYPE PULLUP [get_ports IIC_0_scl_io]

set_property PACKAGE_PIN Y17 [get_ports IIC_0_sda_io]
set_property IOSTANDARD LVCMOS33 [get_ports IIC_0_sda_io]
set_property PULLTYPE PULLUP [get_ports IIC_0_sda_io]


# =============================================================================
# 2. microSD - PS SPI0 through EMIO
# =============================================================================
#
# SCK  = W16
# MOSI = V16
# MISO = Y14
# CS   = W14
#
# =============================================================================

# SCK / CLK
set_property PACKAGE_PIN W16 [get_ports SPI_0_0_sck_io]
set_property IOSTANDARD LVCMOS33 [get_ports SPI_0_0_sck_io]

# MOSI / DI
set_property PACKAGE_PIN V16 [get_ports SPI_0_0_io0_io]
set_property IOSTANDARD LVCMOS33 [get_ports SPI_0_0_io0_io]

# MISO / DO
set_property PACKAGE_PIN Y14 [get_ports SPI_0_0_io1_io]
set_property IOSTANDARD LVCMOS33 [get_ports SPI_0_0_io1_io]

# CS / SS0
set_property PACKAGE_PIN W14 [get_ports SPI_0_0_ss_o]
set_property IOSTANDARD LVCMOS33 [get_ports SPI_0_0_ss_o]
set_property PULLTYPE PULLUP [get_ports SPI_0_0_ss_o]


# =============================================================================
# 3. PS I2C0 through EMIO
# =============================================================================
#
# SCL = T11
# SDA = T10
#
# =============================================================================

set_property PACKAGE_PIN T11 [get_ports IIC_0_0_scl_io]
set_property IOSTANDARD LVCMOS33 [get_ports IIC_0_0_scl_io]
set_property PULLTYPE PULLUP [get_ports IIC_0_0_scl_io]

set_property PACKAGE_PIN T10 [get_ports IIC_0_0_sda_io]
set_property IOSTANDARD LVCMOS33 [get_ports IIC_0_0_sda_io]
set_property PULLTYPE PULLUP [get_ports IIC_0_0_sda_io]


# =============================================================================
# 4. Ethernet MII - TX
# =============================================================================
#
# Current BD ports:
#
#   mii_txd_0[3:0]
#   mii_tx_en_0
#   ENET0_GMII_TX_CLK_0
#
# Board:
#
#   ETH_TXD0  = M14
#   ETH_TXD1  = L15
#   ETH_TXD2  = M15
#   ETH_TXD3  = N15
#   ETH_TXCK  = L14
#   ETH_TXCTL = N16
#
# =============================================================================

# TX data
set_property PACKAGE_PIN M14 [get_ports {mii_txd_0[0]}]
set_property PACKAGE_PIN L15 [get_ports {mii_txd_0[1]}]
set_property PACKAGE_PIN M15 [get_ports {mii_txd_0[2]}]
set_property PACKAGE_PIN N15 [get_ports {mii_txd_0[3]}]

# TX clock
set_property PACKAGE_PIN L14 [get_ports ENET0_GMII_TX_CLK_0]

set_property CLOCK_DEDICATED_ROUTE FALSE [get_nets ENET0_GMII_TX_CLK_0_IBUF]

# TX enable
set_property PACKAGE_PIN N16 [get_ports mii_tx_en_0]

# I/O standard
set_property IOSTANDARD LVCMOS33 [get_ports {mii_txd_0[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {mii_txd_0[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {mii_txd_0[2]}]
set_property IOSTANDARD LVCMOS33 [get_ports {mii_txd_0[3]}]
set_property IOSTANDARD LVCMOS33 [get_ports ENET0_GMII_TX_CLK_0]
set_property IOSTANDARD LVCMOS33 [get_ports mii_tx_en_0]


# =============================================================================
# 5. Ethernet MII - RX
# =============================================================================
#
# Current BD ports:
#
#   mii_rxd_0[3:0]
#   mii_rx_dv_0
#   ENET0_GMII_RX_CLK_0
#
# Board:
#
#   ETH_RXD0  = J14
#   ETH_RXD1  = K14
#   ETH_RXD2  = M18
#   ETH_RXD3  = M17
#   ETH_RXCK  = K17
#   ETH_RXDV  = K18
#
# =============================================================================

# RX data
set_property PACKAGE_PIN J14 [get_ports {mii_rxd_0[0]}]
set_property PACKAGE_PIN K14 [get_ports {mii_rxd_0[1]}]
set_property PACKAGE_PIN M18 [get_ports {mii_rxd_0[2]}]
set_property PACKAGE_PIN M17 [get_ports {mii_rxd_0[3]}]

# RX clock
set_property PACKAGE_PIN K17 [get_ports ENET0_GMII_RX_CLK_0]

# RX data valid
set_property PACKAGE_PIN K18 [get_ports mii_rx_dv_0]

# I/O standard
set_property IOSTANDARD LVCMOS33 [get_ports {mii_rxd_0[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {mii_rxd_0[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {mii_rxd_0[2]}]
set_property IOSTANDARD LVCMOS33 [get_ports {mii_rxd_0[3]}]
set_property IOSTANDARD LVCMOS33 [get_ports ENET0_GMII_RX_CLK_0]
set_property IOSTANDARD LVCMOS33 [get_ports mii_rx_dv_0]


# =============================================================================
# 6. Ethernet MDIO / MDC
# =============================================================================
#
# Current top-level ports:
#
#   MDIO_ETHERNET_0_0_mdc
#   MDIO_ETHERNET_0_0_mdio_io
#
# Board:
#
#   MDC  = G14
#   MDIO = J15
#
# The generated wrapper contains an IOBUF for MDIO.
#
# =============================================================================

set_property PACKAGE_PIN G14 [get_ports MDIO_ETHERNET_0_0_mdc]
set_property IOSTANDARD LVCMOS33 [get_ports MDIO_ETHERNET_0_0_mdc]

set_property PACKAGE_PIN J15 [get_ports MDIO_ETHERNET_0_0_mdio_io]
set_property IOSTANDARD LVCMOS33 [get_ports MDIO_ETHERNET_0_0_mdio_io]


# =============================================================================
# 7. Ethernet PHY RESET
# =============================================================================
#
# Active low:
#
#   0 = PHY reset
#   1 = PHY operating
#
# Board:
#   ETH_nRST = H20
#
# =============================================================================

set_property PACKAGE_PIN H20 [get_ports {ETH_nRST[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ETH_nRST[0]}]


# =============================================================================
# 8. UART1 through EMIO
# =============================================================================
#
# TX = K19
# RX = M19
#
# External USB-UART:
#
#   Zynq TX -> Adapter RX
#   Zynq RX <- Adapter TX
#   GND     -> GND
#
# =============================================================================

set_property PACKAGE_PIN K19 [get_ports UART_1_0_txd]
set_property IOSTANDARD LVCMOS33 [get_ports UART_1_0_txd]

set_property PACKAGE_PIN M19 [get_ports UART_1_0_rxd]
set_property IOSTANDARD LVCMOS33 [get_ports UART_1_0_rxd]


# =============================================================================
# 9. OPTIONAL PULL-UP FOR MDIO
# =============================================================================
#
# Only enable this if the board schematic does NOT already have an
# external MDIO pull-up resistor.
#
# set_property PULLUP true [get_ports MDIO_ETHERNET_0_0_mdio_io]
#
# RTL8201F EVB and many Z7-Lite boards do not populate an external MDIO
# pull-up. Enable the FPGA internal pull-up so the MDIO line idles high
# and the PHY can acknowledge management frames.
# =============================================================================

set_property PULLUP true [get_ports MDIO_ETHERNET_0_0_mdio_io]


# =============================================================================
# 10. Ethernet clock notes
# =============================================================================
#
# Ethernet clocks are NOT generated from FCLK_CLK0/FCLK_CLK1.
#
# PHY -> FPGA:
#
#   ETH_TXCK (L14)
#       |
#       +--> ENET0_GMII_TX_CLK_0
#       |
#       +--> Zynq GEM0 TX clock
#
#   ETH_RXCK (K17)
#       |
#       +--> ENET0_GMII_RX_CLK_0
#       |
#       +--> Zynq GEM0 RX clock
#
# RTL8201F MII clock:
#
#   100 Mbps -> 25 MHz
#   10 Mbps  -> 2.5 MHz
#
# FCLK_CLK0 ~= 50 MHz is used by AXI/IIC logic.
# FCLK_CLK1 ~= 200 MHz is NOT used by Ethernet.
#
# =============================================================================


# =============================================================================
# 11. IMPORTANT - NO OLD RGMII CONSTRAINTS
# =============================================================================
#
# DO NOT add any of the following ports:
#
#   RGMII_0_td
#   RGMII_0_rd
#   RGMII_0_txc
#   RGMII_0_rxc
#   RGMII_0_tx_ctl
#   RGMII_0_rx_ctl
#
# Current architecture is:
#
#   PS GEM0
#      |
#     GMII
#      |
#   gmii_to_mii
#      |
#      MII
#      |
#   RTL8201F
#
# =============================================================================

create_clock -period 40.000 -name eth_tx_clk [get_ports ENET0_GMII_TX_CLK_0]
create_clock -period 40.000 -name eth_rx_clk [get_ports ENET0_GMII_RX_CLK_0]
