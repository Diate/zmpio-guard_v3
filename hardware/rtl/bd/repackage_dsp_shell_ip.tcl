# Idempotent fixup: makes sure mmio_axis_bridge_0 and zmpio_dsp_ctrl_0 in
# design_1.bd are real IP-catalog cells (vendor zmpio, library user, from
# hardware/rtl/ip_repo/) instead of Vivado's create_bd_cell -type module
# -reference stand-in that add_zmpio_dsp_shell.tcl originally used.
#
# Why this is a separate script from add_zmpio_dsp_shell.tcl: design_1.bd
# already has these two cells wired in (see docs/sdd_sad/SDD_10_Step2_PL_Shell.md
# and run_step2_vivado.tcl's own guard that skips re-sourcing
# add_zmpio_dsp_shell.tcl once dsp_core_axis_top_0 exists). Re-running
# add_zmpio_dsp_shell.tcl from scratch against that BD
# would also redo its non-idempotent steps (widening ps7_0_axi_periph
# NUM_MI, the ETH_nRST xlslice rework, etc.) a second time and fail. This
# script only touches the two cells that need to change type, and is safe to
# source repeatedly.
#
# Prerequisite: hardware/rtl/bd/package_zmpio_user_ip.tcl already run once
# (produces hardware/rtl/ip_repo/{mmio_axis_bridge,zmpio_dsp_ctrl}/component.xml).
#
# Assumes project_hub.xpr is already open as [current_project] and
# design_1.bd is the current_bd_design -- matches how run_step2_vivado.tcl
# and add_zmpio_dsp_shell.tcl are both invoked. Safe to source multiple
# times: no-ops if both cells are already the expected IP-catalog VLNV.

set rtl_root [file normalize [file join [file dirname [info script]] ..]]
set ip_repo_dir [file normalize [file join $rtl_root ip_repo]]

foreach d {mmio_axis_bridge zmpio_dsp_ctrl} {
    if {![file exists "$ip_repo_dir/$d/component.xml"]} {
        error "repackage_dsp_shell_ip.tcl: $ip_repo_dir/$d/component.xml not found -- run hardware/rtl/bd/package_zmpio_user_ip.tcl first."
    }
}

set existing_repos [get_property ip_repo_paths [current_project]]
if {[lsearch -exact $existing_repos $ip_repo_dir] == -1} {
    set_property ip_repo_paths [concat $existing_repos [list $ip_repo_dir]] [current_project]
}
update_ip_catalog

set mmio_vlnv zmpio:user:mmio_axis_bridge:1.0
set ctrl_vlnv zmpio:user:zmpio_dsp_ctrl:1.0

proc zmpio_cell_has_vlnv {cell vlnv} {
    if {[llength [get_bd_cells -quiet $cell]] == 0} {
        return 0
    }
    return [expr {[get_property VLNV [get_bd_cells $cell]] eq $vlnv}]
}

if {[zmpio_cell_has_vlnv mmio_axis_bridge_0 $mmio_vlnv] && [zmpio_cell_has_vlnv zmpio_dsp_ctrl_0 $ctrl_vlnv]} {
    puts "repackage_dsp_shell_ip.tcl: mmio_axis_bridge_0/zmpio_dsp_ctrl_0 already packaged IP-catalog cells -- nothing to do."
} else {
    if {[llength [get_bd_cells -quiet dsp_core_axis_top_0]] == 0} {
        error "repackage_dsp_shell_ip.tcl: dsp_core_axis_top_0 not found in design_1.bd -- run add_zmpio_dsp_shell.tcl first to build the Step2 shell from scratch."
    }

    # delete_bd_objs also removes every net/intf-net terminating on these
    # cells, so the rewiring below recreates all of it from scratch -- kept
    # identical to what add_zmpio_dsp_shell.tcl originally wired (sections
    # 3-7), except ps7_0_axi_periph (already NUM_MI=3) and xlconcat_0
    # (already/about to be NUM_PORTS=2 via run_step2_vivado.tcl) are reused
    # as-is rather than re-widened.
    foreach c {mmio_axis_bridge_0 zmpio_dsp_ctrl_0} {
        if {[llength [get_bd_cells -quiet $c]] > 0} {
            delete_bd_objs [get_bd_cells $c]
        }
    }

    create_bd_cell -type ip -vlnv $mmio_vlnv mmio_axis_bridge_0
    create_bd_cell -type ip -vlnv $ctrl_vlnv zmpio_dsp_ctrl_0

    # ---- clock/reset (add_zmpio_dsp_shell.tcl section 3) ----
    foreach {cell aclk_port aresetn_port} {
        mmio_axis_bridge_0 s_axi_aclk s_axi_aresetn
        zmpio_dsp_ctrl_0   s_axi_aclk s_axi_aresetn
    } {
        connect_bd_net [get_bd_pins processing_system7_0/FCLK_CLK0] [get_bd_pins $cell/$aclk_port]
        connect_bd_net [get_bd_pins rst_ps7_0_49M/peripheral_aresetn] [get_bd_pins $cell/$aresetn_port]
    }
    # This script keeps wiring the same second clock port (m_axis_aclk /
    # s_axis_aclk) rather than reworking the ASSOCIATED_BUSIF association --
    # packaging/cell-type is the only thing meant to change here, not the
    # net topology.
    connect_bd_net [get_bd_pins processing_system7_0/FCLK_CLK0] [get_bd_pins mmio_axis_bridge_0/m_axis_aclk]
    connect_bd_net [get_bd_pins processing_system7_0/FCLK_CLK0] [get_bd_pins zmpio_dsp_ctrl_0/s_axis_aclk]

    # ---- AXIS datapath (add_zmpio_dsp_shell.tcl section 4) ----
    connect_bd_intf_net [get_bd_intf_pins mmio_axis_bridge_0/m_axis] \
        [get_bd_intf_pins dsp_core_axis_top_0/sample_axis_in]
    connect_bd_intf_net [get_bd_intf_pins dsp_core_axis_top_0/feature_axis_out] \
        [get_bd_intf_pins zmpio_dsp_ctrl_0/s_axis]

    # ---- AXI4-Lite (add_zmpio_dsp_shell.tcl section 5, minus the NUM_MI
    # widening -- ps7_0_axi_periph is already 1x3 from the original run) ----
    connect_bd_intf_net [get_bd_intf_pins ps7_0_axi_periph/M01_AXI] \
        [get_bd_intf_pins mmio_axis_bridge_0/s_axi]
    connect_bd_intf_net [get_bd_intf_pins ps7_0_axi_periph/M02_AXI] \
        [get_bd_intf_pins zmpio_dsp_ctrl_0/s_axi]

    # ---- IRQ (add_zmpio_dsp_shell.tcl section 6, minus the NUM_PORTS
    # widening). run_step2_vivado.tcl deletes+recreates xlconcat_0 from
    # scratch right after sourcing this script and rewires this same net, so
    # this connection is belt-and-suspenders for anyone sourcing this file
    # standalone rather than via run_step2_vivado.tcl. ----
    if {[llength [get_bd_cells -quiet xlconcat_0]] > 0} {
        connect_bd_net [get_bd_pins zmpio_dsp_ctrl_0/irq_out] [get_bd_pins xlconcat_0/In1]
    }

    # ---- AXI4-Lite address segments (add_zmpio_dsp_shell.tcl section 7) ----
    assign_bd_address [get_bd_addr_segs {mmio_axis_bridge_0/s_axi/reg0}]
    assign_bd_address [get_bd_addr_segs {zmpio_dsp_ctrl_0/s_axi/reg0}]

    validate_bd_design
    save_bd_design
    puts "repackage_dsp_shell_ip.tcl: mmio_axis_bridge_0/zmpio_dsp_ctrl_0 converted to IP-catalog cells ($mmio_vlnv, $ctrl_vlnv) and rewired."
}
