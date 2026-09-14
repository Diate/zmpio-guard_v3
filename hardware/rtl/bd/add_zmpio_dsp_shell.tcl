# Integrates the Step2 PL shell into design_1.bd: the dsp_core_axis_top HLS IP,
# mmio_axis_bridge.v and zmpio_dsp_ctrl.v. See docs/SDD/SDD_06_PL_DSP_SHELL.md.
#
# This performs structural block-design surgery -- it adds custom IP, expands the
# AXI interconnect and widens the PS7 interrupt concat -- and it edits
# design_1.bd in place. Back up hardware/vivado/project_hub before running.
#
# Prerequisites:
#   1. Run hardware/rtl/dsp_core/hls/run_hls_axis.tcl under vitis_hls. It exports a
#      packaged IP (containing component.xml) under
#      <hls_run_dir>/dsp_core_axis_hls_proj/solution1/impl/ip/. Copy that
#      directory's contents into hardware/rtl/ip_repo/dsp_core_axis_top/, which is
#      version controlled and already an ip_repo_path of project_hub.xpr.
#      HLS_IP_REPO_DIR must point at that permanent copy, never at a transient
#      HLS run directory: if the referenced VLNV disappears, the next rebuild
#      fails with dsp_core_axis_top_0 locked ("IP definition not found").
#   1b. Run hardware/rtl/bd/package_zmpio_user_ip.tcl (needs no open project) to
#      produce hardware/rtl/ip_repo/{mmio_axis_bridge,zmpio_dsp_ctrl}/component.xml.
#      Both cells are instantiated as IP-catalog cells, not as
#      `create_bd_cell -type module -reference`: the module-reference form writes
#      no component.xml and cannot declare zmpio_dsp_ctrl_0/irq_out as an
#      interrupt, which leaves pl.dtsi without `interrupts` for it.
#   2. Open hardware/vivado/project_hub/project_hub.xpr first. This script assumes
#      the project is already open as [current_project].
#
# Usage:
#   vivado -mode batch -source hardware/rtl/bd/add_zmpio_dsp_shell.tcl
# or paste into the Vivado Tcl console with the project already open.

# ---------------------------------------------------------------------
# Permanent, checked-in copy of the Vitis-HLS-exported IP (see prerequisite
# 1 above) -- must contain component.xml directly.
# ---------------------------------------------------------------------
set HLS_IP_REPO_DIR [file normalize [file join [file dirname [info script]] .. ip_repo dsp_core_axis_top]]

# Script lives at hardware/rtl/bd/ -- its parent (hardware/rtl/) is the root
# that holds src/ and constraints/.
set rtl_root [file normalize [file join [file dirname [info script]] ..]]
set verilog_src_dir [file normalize [file join $rtl_root src]]
set xdc_path [file normalize [file join $rtl_root constraints zmpio_v2.xdc]]
set zmpio_ip_repo_dir [file normalize [file join $rtl_root ip_repo]]

if {![file exists "$HLS_IP_REPO_DIR/component.xml"]} {
    error "HLS_IP_REPO_DIR does not point at a packaged IP (no component.xml under $HLS_IP_REPO_DIR). Run hardware/rtl/dsp_core/hls/run_hls_axis.tcl first and fix the path at the top of this script."
}
foreach d {mmio_axis_bridge zmpio_dsp_ctrl} {
    if {![file exists "$zmpio_ip_repo_dir/$d/component.xml"]} {
        error "$zmpio_ip_repo_dir/$d/component.xml not found. Run hardware/rtl/bd/package_zmpio_user_ip.tcl first."
    }
}

set bd_design [get_bd_designs design_1]
if {[llength $bd_design] == 0} {
    # In batch/Tcl-console project mode, opening the .xpr does not also load
    # the block design into memory the way double-clicking it in the GUI
    # does -- do that explicitly.
    set bd_files [get_files -quiet design_1.bd]
    if {[llength $bd_files] == 0} {
        error "design_1.bd not found in the open project -- open hardware/vivado/project_hub/project_hub.xpr first."
    }
    open_bd_design [lindex $bd_files 0]
    set bd_design [get_bd_designs design_1]
}
current_bd_design $bd_design

# ---- 1. Bring in the two RTL sources + the packaged HLS IP ----
add_files -norecurse [list \
    "$verilog_src_dir/mmio_axis_bridge.v" \
    "$verilog_src_dir/zmpio_dsp_ctrl.v" \
]
if {[file exists $xdc_path]} {
    add_files -fileset constrs_1 -norecurse $xdc_path
}

set existing_repos [get_property ip_repo_paths [current_project]]
set_property ip_repo_paths [concat $existing_repos [list $HLS_IP_REPO_DIR $zmpio_ip_repo_dir]] [current_project]
update_ip_catalog

# ---- 2. Instantiate the three new cells ----
# mmio_axis_bridge_0/zmpio_dsp_ctrl_0 are real IP-catalog cells (packaged by
# package_zmpio_user_ip.tcl), not create_bd_cell -type module -reference --
# see the prerequisite note (1b) at the top of this file.
create_bd_cell -type ip -vlnv zmpio:dsp_core:dsp_core_axis_top:1.0 dsp_core_axis_top_0
create_bd_cell -type ip -vlnv zmpio:user:mmio_axis_bridge:1.0 mmio_axis_bridge_0
create_bd_cell -type ip -vlnv zmpio:user:zmpio_dsp_ctrl:1.0 zmpio_dsp_ctrl_0

# ---- 3. Clock/reset: same FCLK0 domain as axi_iic_0 ----
foreach {cell aclk_port aresetn_port} {
    mmio_axis_bridge_0 s_axi_aclk s_axi_aresetn
    zmpio_dsp_ctrl_0   s_axi_aclk s_axi_aresetn
} {
    connect_bd_net [get_bd_pins processing_system7_0/FCLK_CLK0] [get_bd_pins $cell/$aclk_port]
    connect_bd_net [get_bd_pins rst_ps7_0_49M/peripheral_aresetn] [get_bd_pins $cell/$aresetn_port]
}
# create_bd_cell's HDL-name-based auto-inference only associates s_axi_aclk
# with the s_axi (AXI4-Lite) bus for these two hand-written modules; it does
# not know the AXIS bus each one also carries shares the same physical
# clock. mmio_axis_bridge.v/zmpio_dsp_ctrl.v each carry a second,
# otherwise-unused clock port (m_axis_aclk / s_axis_aclk) whose name prefix
# matches the AXIS bus, which Vivado's naming convention picks up
# automatically instead of relying on ASSOCIATED_BUSIF. Both still need
# wiring to the same physical clock as everything else here.
connect_bd_net [get_bd_pins processing_system7_0/FCLK_CLK0] [get_bd_pins mmio_axis_bridge_0/m_axis_aclk]
connect_bd_net [get_bd_pins processing_system7_0/FCLK_CLK0] [get_bd_pins zmpio_dsp_ctrl_0/s_axis_aclk]
# dsp_core_axis_top is ap_ctrl_none (free-running) -- HLS-generated clock/
# reset port names are ap_clk/ap_rst_n by default.
connect_bd_net [get_bd_pins processing_system7_0/FCLK_CLK0] [get_bd_pins dsp_core_axis_top_0/ap_clk]
connect_bd_net [get_bd_pins rst_ps7_0_49M/peripheral_aresetn] [get_bd_pins dsp_core_axis_top_0/ap_rst_n]

# ---- 4. AXIS datapath: bridge -> dsp core -> ctrl/FIFO ----
connect_bd_intf_net [get_bd_intf_pins mmio_axis_bridge_0/m_axis] \
    [get_bd_intf_pins dsp_core_axis_top_0/sample_axis_in]
connect_bd_intf_net [get_bd_intf_pins dsp_core_axis_top_0/feature_axis_out] \
    [get_bd_intf_pins zmpio_dsp_ctrl_0/s_axis]

# ---- 5. AXI4-Lite: expand ps7_0_axi_periph 1x1 -> 1x3 ----
set_property CONFIG.NUM_MI {3} [get_bd_cells ps7_0_axi_periph]
connect_bd_intf_net [get_bd_intf_pins ps7_0_axi_periph/M01_AXI] \
    [get_bd_intf_pins mmio_axis_bridge_0/s_axi]
connect_bd_intf_net [get_bd_intf_pins ps7_0_axi_periph/M02_AXI] \
    [get_bd_intf_pins zmpio_dsp_ctrl_0/s_axi]
# Widening NUM_MI adds new per-master clock/reset pins on the interconnect
# itself (M01_ACLK/M01_ARESETN, M02_ACLK/M02_ARESETN) -- these are separate
# from the S_AXI-side ports already wired on the two new peripherals above,
# and validate_bd_design fails without them ("clock pins not connected").
foreach idx {M01 M02} {
    connect_bd_net [get_bd_pins processing_system7_0/FCLK_CLK0] \
        [get_bd_pins ps7_0_axi_periph/${idx}_ACLK]
    connect_bd_net [get_bd_pins rst_ps7_0_49M/peripheral_aresetn] \
        [get_bd_pins ps7_0_axi_periph/${idx}_ARESETN]
}

# ---- 6. IRQ: expand xlconcat_0 1 -> 2, wire into PS7 IRQ_F2P ----
set_property CONFIG.NUM_PORTS {2} [get_bd_cells xlconcat_0]
connect_bd_net [get_bd_pins zmpio_dsp_ctrl_0/irq_out] [get_bd_pins xlconcat_0/In1]
# PCW_NUM_F2P_INTR_INPUTS is read-only via set_property on this PS7 IP
# version -- widening xlconcat_0/dout to 2 bits and (re)connecting it to
# IRQ_F2P is what actually drives the PS7 port width; re-make the net so
# Vivado re-evaluates the width from the wider driver.
disconnect_bd_net [get_bd_nets xlconcat_0_dout] [get_bd_pins processing_system7_0/IRQ_F2P]
connect_bd_net [get_bd_pins xlconcat_0/dout] [get_bd_pins processing_system7_0/IRQ_F2P]

# ---- 7. Auto-assign the two new AXI4-Lite address segments ----
assign_bd_address [get_bd_addr_segs {mmio_axis_bridge_0/s_axi/reg0}]
assign_bd_address [get_bd_addr_segs {zmpio_dsp_ctrl_0/s_axi/reg0}]

# ---- 8. Pre-existing baseline drift, unrelated to the Step2 shell above ----
# design_1.bd wires the external ETH_nRST port straight to
# processing_system7_0/GPIO_O -- the PS7's plain MIO GPIO output, which is
# ALWAYS 64 bits wide regardless of any PCW_* property (that first attempt,
# setting PCW_GPIO_EMIO_GPIO_WIDTH, was a dead end: GPIO_O is not EMIO and
# isn't sized by that property at all). constaint.xdc only ever constrains
# ETH_nRST[0] (PACKAGE_PIN H20) -- the other 63 bits of the port are
# electrically meaningless. This is harmless as long as nothing forces a
# full resynthesis (the last checked-in bitstream/synth cache predates
# this), but a clean `reset_run synth_1` (as this script's caller does) hits
# "IO placer failed to find a solution" on the 63 unconstrained bits.
# Fix: narrow the external port itself to 1 bit and rewire it (and the
# pre-existing ila_0/probe4 debug tap that shared the same net) straight
# back to GPIO_O -- this fix must be applied to hardware/vivado/project_hub's
# own design_1.bd directly (not just to an isolated working copy), since a
# from-scratch `reset_run synth_1` on the baseline hits the same "IO placer
# failed" error whenever ETH_nRST is still wired straight to GPIO_O. Check
# for that before assuming this script will run clean on the first try.
# Neither a plain `create_bd_port -dir O ETH_nRST` nor an explicit
# `-from 0 -to 0` survives: confirmed by testing that validate_bd_design /
# generate_target's net "parameter propagation" silently re-widens a BD
# port's width to match whatever wider net/pin it's directly wired to --
# only a genuinely fixed-width IP pin (like ila_0/probe4 below) is exempt
# and gets a real width-mismatch warning instead of a silent re-widen. So a
# bare net from GPIO_O(64) can never terminate in a real 1-bit port. Fix:
# insert an xlslice IP (a real IP with a locked-width output pin) to take
# just bit 0 of GPIO_O, and drive ETH_nRST from that.
delete_bd_objs [get_bd_nets processing_system7_0_GPIO_O] [get_bd_ports ETH_nRST]
create_bd_port -dir O ETH_nRST
create_bd_cell -type ip -vlnv xilinx.com:ip:xlslice:1.0 eth_nrst_slice_0
set_property -dict [list \
    CONFIG.DIN_WIDTH {64} CONFIG.DIN_FROM {0} CONFIG.DIN_TO {0} CONFIG.DOUT_WIDTH {1} \
] [get_bd_cells eth_nrst_slice_0]
connect_bd_net [get_bd_pins processing_system7_0/GPIO_O] \
    [get_bd_pins eth_nrst_slice_0/Din] [get_bd_pins ila_0/probe4]
connect_bd_net [get_bd_pins eth_nrst_slice_0/Dout] [get_bd_ports ETH_nRST]

validate_bd_design
save_bd_design

puts "add_zmpio_dsp_shell.tcl: BD updated. Next: generate_target + create_hdl_wrapper (or let 'Generate Bitstream' do both), then Generate Bitstream -> Export Hardware. Re-check the two AXI-Lite base addresses and the new IRQ number afterwards in firmware/platform_dual/hw/sdt/pl.dtsi and update firmware/app_freertos/src/fpga_dsp_hal.h's *_TBD macros to match."
