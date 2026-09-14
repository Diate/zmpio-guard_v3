# Integrates zmpio_doorbell (docs/ROADMAP.md STEP 5, docs/sdd_sad/
# SDD_DOORBELL.md) into design_1.bd -- a one-way CPU1->CPU0 wakeup doorbell,
# on top of the Step2 PL shell (add_zmpio_dsp_shell.tcl, already in the BD:
# mmio_axis_bridge_0 on M01_AXI, zmpio_dsp_ctrl_0 on M02_AXI, xlconcat_0
# widened to 2 ports for zmpio_dsp_ctrl_0/irq_out on In1 = GIC SPI 62).
#
# This script only adds the doorbell peripheral -- it does not touch the
# Step2 datapath (mmio_axis_bridge_0/dsp_core_axis_top_0/zmpio_dsp_ctrl_0) at
# all, matching Step 5's scope: no ABI v3 upgrade, no change to the existing
# PL shell.
#
# Prerequisites:
#   1. hardware/rtl/bd/add_zmpio_dsp_shell.tcl already applied (Step 2 PASS
#      on the board this BD came from).
#   2. Run hardware/rtl/bd/package_zmpio_user_ip.tcl first (packages
#      zmpio_doorbell as real IP-catalog IP, vendor zmpio, with irq_out
#      declared as an interrupt bus interface -- see that script's header
#      for why create_bd_cell -type module -reference is not used here
#      either: same pl.dtsi `interrupts` problem as zmpio_dsp_ctrl).
#   3. Open hardware/vivado/project_hub/project_hub.xpr in Vivado first (GUI
#      or `vivado -mode batch -source add_zmpio_doorbell.tcl` from within
#      that project's directory) -- this script assumes the project is
#      already open as [current_project].
#
# Usage:
#   vivado -mode batch -source hardware/rtl/bd/add_zmpio_doorbell.tcl
# or paste into the Vivado Tcl console with hardware/vivado/project_hub already open.

set rtl_root [file normalize [file join [file dirname [info script]] ..]]
set zmpio_ip_repo_dir [file normalize [file join $rtl_root ip_repo]]

if {![file exists "$zmpio_ip_repo_dir/zmpio_doorbell/component.xml"]} {
    error "$zmpio_ip_repo_dir/zmpio_doorbell/component.xml not found. Run hardware/rtl/bd/package_zmpio_user_ip.tcl first."
}

set bd_design [get_bd_designs design_1]
if {[llength $bd_design] == 0} {
    set bd_files [get_files -quiet design_1.bd]
    if {[llength $bd_files] == 0} {
        error "design_1.bd not found in the open project -- open hardware/vivado/project_hub/project_hub.xpr first."
    }
    open_bd_design [lindex $bd_files 0]
    set bd_design [get_bd_designs design_1]
}
current_bd_design $bd_design

foreach required_cell {zmpio_dsp_ctrl_0 xlconcat_0 ps7_0_axi_periph processing_system7_0} {
    if {[llength [get_bd_cells -quiet $required_cell]] == 0} {
        error "add_zmpio_doorbell.tcl: expected cell '$required_cell' already in design_1.bd -- run add_zmpio_dsp_shell.tcl (Step 2) first."
    }
}

set existing_repos [get_property ip_repo_paths [current_project]]
if {[lsearch -exact $existing_repos $zmpio_ip_repo_dir] == -1} {
    set_property ip_repo_paths [concat $existing_repos [list $zmpio_ip_repo_dir]] [current_project]
}
update_ip_catalog

# ---- 1. Instantiate the doorbell cell ----
create_bd_cell -type ip -vlnv zmpio:user:zmpio_doorbell:1.0 zmpio_doorbell_0

# ---- 2. Clock/reset: same FCLK0 domain as the rest of the Step2 shell ----
connect_bd_net [get_bd_pins processing_system7_0/FCLK_CLK0] [get_bd_pins zmpio_doorbell_0/s_axi_aclk]
connect_bd_net [get_bd_pins rst_ps7_0_49M/peripheral_aresetn] [get_bd_pins zmpio_doorbell_0/s_axi_aresetn]

# ---- 3. AXI4-Lite: expand ps7_0_axi_periph to one more master port ----
# NUM_MI is read back rather than assumed to be 3 -- this script must also
# work against a BD where some other peripheral was added between Step 2 and
# Step 5.
set current_num_mi [get_property CONFIG.NUM_MI [get_bd_cells ps7_0_axi_periph]]
set new_mi_index [format "M%02d" $current_num_mi]
set_property CONFIG.NUM_MI [expr {$current_num_mi + 1}] [get_bd_cells ps7_0_axi_periph]
connect_bd_intf_net [get_bd_intf_pins ps7_0_axi_periph/${new_mi_index}_AXI] \
    [get_bd_intf_pins zmpio_doorbell_0/s_axi]
connect_bd_net [get_bd_pins processing_system7_0/FCLK_CLK0] \
    [get_bd_pins ps7_0_axi_periph/${new_mi_index}_ACLK]
connect_bd_net [get_bd_pins rst_ps7_0_49M/peripheral_aresetn] \
    [get_bd_pins ps7_0_axi_periph/${new_mi_index}_ARESETN]

# ---- 4. IRQ: widen xlconcat_0 by ONE more port, wire into PS7 IRQ_F2P ----
# NOT a plain `set_property CONFIG.NUM_PORTS` on the existing cell -- that
# in-place mutation on an already-customized xlconcat is exactly what
# run_step2_vivado.tcl documents as broken (dout_width silently stays stuck
# at the OLD width in the generated synth wrapper even though the property
# itself reads back correctly, confirmed there via CRITICAL WARNING
# [BD 41-2383]). run_step2_vivado.tcl's proven fix -- delete xlconcat_0 and
# recreate it from scratch with the final NUM_PORTS set as part of the
# initial customization -- is applied here directly instead of repeating
# add_zmpio_dsp_shell.tcl's original (buggy) in-place-widen attempt and then
# needing a second driver-script pass to fix it.
#
# Capture every existing In<N> driver before deleting the cell, so all of
# them (not just the new doorbell one) get rewired afterward -- this script
# must not assume there are exactly 2 today (axi_iic_0, zmpio_dsp_ctrl_0);
# it works for however many are actually there.
set current_num_ports [get_property CONFIG.NUM_PORTS [get_bd_cells xlconcat_0]]
set existing_irq_drivers {}
for {set idx 0} {$idx < $current_num_ports} {incr idx} {
    set in_pin [get_bd_pins xlconcat_0/In${idx}]
    set net [get_bd_nets -of_objects $in_pin]
    set driver_pin [lindex [lsearch -inline -all -not -exact \
        [get_bd_pins -quiet -of_objects $net] $in_pin] 0]
    if {$driver_pin eq ""} {
        error "add_zmpio_doorbell.tcl: xlconcat_0/In${idx} has no driver -- cannot safely rebuild xlconcat_0."
    }
    lappend existing_irq_drivers $driver_pin
}
set new_num_ports [expr {$current_num_ports + 1}]
set new_in_index "In${current_num_ports}"

delete_bd_objs [get_bd_cells xlconcat_0]
create_bd_cell -type ip -vlnv xilinx.com:ip:xlconcat:2.1 xlconcat_0
set_property CONFIG.NUM_PORTS $new_num_ports [get_bd_cells xlconcat_0]
for {set idx 0} {$idx < $current_num_ports} {incr idx} {
    connect_bd_net [lindex $existing_irq_drivers $idx] [get_bd_pins xlconcat_0/In${idx}]
}
connect_bd_net [get_bd_pins zmpio_doorbell_0/irq_out] [get_bd_pins xlconcat_0/$new_in_index]
connect_bd_net [get_bd_pins xlconcat_0/dout] [get_bd_pins processing_system7_0/IRQ_F2P]

# ---- 5. Auto-assign the new AXI4-Lite address segment ----
assign_bd_address [get_bd_addr_segs {zmpio_doorbell_0/s_axi/reg0}]

validate_bd_design
save_bd_design

puts "add_zmpio_doorbell.tcl: BD updated (zmpio_doorbell_0 on $new_mi_index, irq_out on xlconcat_0/$new_in_index -> GIC SPI [expr {61 + $current_num_ports}])."
puts "Next: Generate Bitstream -> Export Hardware. Re-check the new AXI-Lite base address and the new IRQ number afterwards in firmware/platform_dual/hw/sdt/pl.dtsi, and update pl_doorbell.h's *_TBD macros on both CPU0 and CPU1 to match."
