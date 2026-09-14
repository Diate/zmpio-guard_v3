# One-time structural fixup for the Step 3 fault-injection requirement: a
# single DSP fault must not take down I2C/SD/scheduler (fault injection
# scenarios: FIFO full, PL reset mid-RUN).
#
# zmpio_dsp_ctrl.v gained a real output port, dsp_soft_rst_n, driven by
# CONTROL.SOFT_RESET (bit1) -- see that file's header comment. This script
# rewires dsp_core_axis_top_0/ap_rst_n and mmio_axis_bridge_0/s_axi_aresetn
# to that new signal INSTEAD of rst_ps7_0_49M/peripheral_aresetn, so a
# CPU1-triggered (or JTAG-triggered, via a raw mwr to zmpio_dsp_ctrl_0's
# CONTROL register) soft-reset only faults the DSP pipeline. axi_iic_0 (MPU6050
# I2C, owned by CPU1) and zmpio_dsp_ctrl_0's own AXI4-Lite slave interface
# stay on the shared rst_ps7_0_49M network, untouched -- exactly the
# separation the fault-injection gate needs (a DSP-only fault must not also
# take down I2C).
#
# Prerequisite: hardware/rtl/bd/package_zmpio_user_ip.tcl already re-run
# against the current zmpio_dsp_ctrl.v (the one with dsp_soft_rst_n) --
# component.xml must list the new port before update_ip_catalog will see it.
#
# Assumes project_hub.xpr is open and design_1.bd is current_bd_design --
# matches how add_zmpio_dsp_shell.tcl / repackage_dsp_shell_ip.tcl are
# invoked. NOT idempotent by design (one-time structural change): re-running
# after it already succeeded is a no-op guarded by the dsp_soft_rst_n pin
# check below, same guard style as repackage_dsp_shell_ip.tcl.

set rtl_root [file normalize [file join [file dirname [info script]] ..]]
set ip_repo_dir [file normalize [file join $rtl_root ip_repo]]

if {![file exists "$ip_repo_dir/zmpio_dsp_ctrl/component.xml"]} {
    error "wire_dsp_soft_reset.tcl: $ip_repo_dir/zmpio_dsp_ctrl/component.xml not found -- run hardware/rtl/bd/package_zmpio_user_ip.tcl first."
}

set existing_repos [get_property ip_repo_paths [current_project]]
if {[lsearch -exact $existing_repos $ip_repo_dir] == -1} {
    set_property ip_repo_paths [concat $existing_repos [list $ip_repo_dir]] [current_project]
}
update_ip_catalog

if {[llength [get_bd_pins -quiet zmpio_dsp_ctrl_0/dsp_soft_rst_n]] > 0} {
    puts "wire_dsp_soft_reset.tcl: zmpio_dsp_ctrl_0/dsp_soft_rst_n already present and (presumably) wired -- nothing to do."
} else {
    if {[llength [get_bd_cells -quiet zmpio_dsp_ctrl_0]] == 0} {
        error "wire_dsp_soft_reset.tcl: zmpio_dsp_ctrl_0 not found in design_1.bd -- run add_zmpio_dsp_shell.tcl / repackage_dsp_shell_ip.tcl first."
    }

    # Delete + recreate to force Vivado to re-read component.xml for this
    # cell (same VLNV, grown port list) -- same technique
    # repackage_dsp_shell_ip.tcl uses for a cell-type swap. delete_bd_objs
    # also removes every net/intf-net terminating on this cell, so all of its
    # own connections are rebuilt from scratch below (identical to what
    # add_zmpio_dsp_shell.tcl / repackage_dsp_shell_ip.tcl already wire).
    delete_bd_objs [get_bd_cells zmpio_dsp_ctrl_0]
    create_bd_cell -type ip -vlnv zmpio:user:zmpio_dsp_ctrl:1.0 zmpio_dsp_ctrl_0

    connect_bd_net [get_bd_pins processing_system7_0/FCLK_CLK0] [get_bd_pins zmpio_dsp_ctrl_0/s_axi_aclk]
    connect_bd_net [get_bd_pins rst_ps7_0_49M/peripheral_aresetn] [get_bd_pins zmpio_dsp_ctrl_0/s_axi_aresetn]
    connect_bd_net [get_bd_pins processing_system7_0/FCLK_CLK0] [get_bd_pins zmpio_dsp_ctrl_0/s_axis_aclk]
    connect_bd_intf_net [get_bd_intf_pins dsp_core_axis_top_0/feature_axis_out] \
        [get_bd_intf_pins zmpio_dsp_ctrl_0/s_axis]
    connect_bd_intf_net [get_bd_intf_pins ps7_0_axi_periph/M02_AXI] \
        [get_bd_intf_pins zmpio_dsp_ctrl_0/s_axi]
    connect_bd_net [get_bd_pins zmpio_dsp_ctrl_0/irq_out] [get_bd_pins xlconcat_0/In1]
    assign_bd_address [get_bd_addr_segs {zmpio_dsp_ctrl_0/s_axi/reg0}]

    # ---- the actual point of this script: independent DSP-shell reset ----
    set old_net [get_bd_nets -quiet -of_objects [get_bd_pins dsp_core_axis_top_0/ap_rst_n]]
    if {[llength $old_net] > 0} {
        disconnect_bd_net $old_net [get_bd_pins dsp_core_axis_top_0/ap_rst_n]
    }
    set old_net [get_bd_nets -quiet -of_objects [get_bd_pins mmio_axis_bridge_0/s_axi_aresetn]]
    if {[llength $old_net] > 0} {
        disconnect_bd_net $old_net [get_bd_pins mmio_axis_bridge_0/s_axi_aresetn]
    }
    connect_bd_net [get_bd_pins zmpio_dsp_ctrl_0/dsp_soft_rst_n] [get_bd_pins dsp_core_axis_top_0/ap_rst_n]
    connect_bd_net [get_bd_pins zmpio_dsp_ctrl_0/dsp_soft_rst_n] [get_bd_pins mmio_axis_bridge_0/s_axi_aresetn]

    validate_bd_design
    save_bd_design
    puts "wire_dsp_soft_reset.tcl: dsp_core_axis_top_0/ap_rst_n and mmio_axis_bridge_0/s_axi_aresetn now driven by zmpio_dsp_ctrl_0/dsp_soft_rst_n, independent of rst_ps7_0_49M/axi_iic_0."
}
