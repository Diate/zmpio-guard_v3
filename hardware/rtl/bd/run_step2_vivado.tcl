# Driver script: opens project_hub, applies add_zmpio_dsp_shell.tcl, builds the
# bitstream and exports a new hardware platform.
#
#   vivado -mode batch -source hardware/rtl/bd/run_step2_vivado.tcl
#
# Runs from any directory -- every path is resolved through project_paths.tcl.
# This edits design_1.bd in place; back up hardware/vivado/project_hub first.

# Repository paths -- resolved from this script's own location.
source [file join [file dirname [info script]] project_paths.tcl]
set project_xpr $zmpio(project_xpr)
set out_xsa $zmpio(out_xsa)

open_project $project_xpr

# add_zmpio_dsp_shell.tcl mutates design_1.bd on disk (create_bd_cell +
# save_bd_design) -- re-sourcing it against a design_1.bd that already has
# those cells (e.g. re-running this driver after a later step failed) would
# error out trying to create cells that already exist. Only apply it once.
set bd_files [get_files -quiet design_1.bd]
if {[llength $bd_files] > 0} {
    open_bd_design [lindex $bd_files 0]
}
if {[llength [get_bd_cells -quiet dsp_core_axis_top_0]] == 0} {
    source [file join $zmpio(bd_dir) add_zmpio_dsp_shell.tcl]
} else {
    puts "run_step2_vivado.tcl: design_1.bd already has the Step2 DSP shell cells -- skipping add_zmpio_dsp_shell.tcl."
}

# Idempotent fixup, run unconditionally regardless of which branch above ran:
# converts mmio_axis_bridge_0/zmpio_dsp_ctrl_0 to real IP-catalog cells if
# they aren't already; no-ops if add_zmpio_dsp_shell.tcl above already
# created them as catalog IP. Requires
# hardware/rtl/bd/package_zmpio_user_ip.tcl to have been run first.
source [file join $zmpio(bd_dir) repackage_dsp_shell_ip.tcl]

update_compile_order -fileset sources_1

# design_1.bd can end up marked AutoDisabled in the sources_1 fileset when a
# project is copied/imported without re-running "Generate Output Products" --
# Vivado's TopAutoSet then silently falls back to whichever plain RTL file
# looks most top-level (gmii_to_mii, a leaf Ethernet helper module), so
# synth_1/impl_1 would build gmii_to_mii alone and never touch the real
# design_1_wrapper hierarchy at all. Re-enable the BD, regenerate/create its
# HDL wrapper, and set it as the explicit top.
set_property is_enabled true [get_files design_1.bd]

# Widening an already-customized xlconcat_0 in place via a post-hoc
# `set_property CONFIG.NUM_PORTS {2}` (as add_zmpio_dsp_shell.tcl does) does
# not reliably re-trigger the IP's derived-dout_width recompute: dout can
# stay stuck at 1 bit (IN0_WIDTH/IN1_WIDTH both correctly show 1 with
# resolve_type "propagated", but the dout_width parameter baked into the
# generated synth wrapper stays 1) even after `generate_target all
# [get_files design_1.bd] -force`, producing CRITICAL WARNING [BD 41-2383]
# (IRQ_F2P(2) to xlconcat_0/dout(1)) and a synth-time "Port In1[0] ...
# unconnected or has no load". Fix: delete xlconcat_0 and recreate it from
# scratch with NUM_PORTS=2 set as part of the initial customization, then
# rewire the same three nets. Idempotent -- safe to run every time
# regardless of prior xlconcat_0 state.
if {[llength [get_bd_cells -quiet xlconcat_0]] > 0} {
    delete_bd_objs [get_bd_cells xlconcat_0]
}
create_bd_cell -type ip -vlnv xilinx.com:ip:xlconcat:2.1 xlconcat_0
set_property CONFIG.NUM_PORTS {2} [get_bd_cells xlconcat_0]
connect_bd_net [get_bd_pins axi_iic_0/iic2intc_irpt] [get_bd_pins xlconcat_0/In0]
connect_bd_net [get_bd_pins zmpio_dsp_ctrl_0/irq_out] [get_bd_pins xlconcat_0/In1]
connect_bd_net [get_bd_pins xlconcat_0/dout] [get_bd_pins processing_system7_0/IRQ_F2P]
validate_bd_design
save_bd_design

generate_target all [get_files design_1.bd] -force
if {[llength [get_files -quiet design_1_wrapper.v]] == 0} {
    add_files -norecurse [make_wrapper -files [get_files design_1.bd] -top]
}
set_property top design_1_wrapper [current_fileset]
update_compile_order -fileset sources_1

# synth_1's IncrementalCheckpoint property (see project_hub.xpr) can point at
# a stale design_1_wrapper.dcp imported from a differently-named project
# ("project_1" per that .dcp's ImportPath attribute). Reusing it via
# incremental synthesis against the Step2-modified design_1.bd makes Vivado
# attempt a redundant standalone implementation+write_bitstream of the
# pre-existing gmii_to_mii OOC module (unrelated to this Step2 change),
# which then fails DRC (NSTD-1/UCIO-1) because that internal, pin-less
# submodule was never meant to go through a top-level bitstream DRC pass on
# its own. A fresh checkout has no valid prior build to incrementally build
# upon anyway, so just do a full synthesis instead of chasing that stale
# checkpoint.
set_property AUTO_INCREMENTAL_CHECKPOINT 0 [get_runs synth_1]

reset_run synth_1
launch_runs synth_1 -jobs 4
wait_on_run synth_1
if {[get_property PROGRESS [get_runs synth_1]] != "100%"} {
    error "synth_1 did not complete successfully"
}

# gmii_to_mii (pre-existing module_ref cell, unrelated to Step2) is
# implemented by Vivado as its own standalone run with purely-internal
# ports, and write_bitstream's DRC (NSTD-1/UCIO-1) refuses to run against
# unconstrained ports -- expected for an OOC-style module with no package
# pins of its own. See suppress_ooc_io_drc.tcl for why this is safe (the
# real top-level ports stay fully constrained).
set_property STEPS.WRITE_BITSTREAM.TCL.PRE \
    [file join $zmpio(bd_dir) suppress_ooc_io_drc.tcl] [get_runs impl_1]

launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1
if {[get_property PROGRESS [get_runs impl_1]] != "100%"} {
    error "impl_1 (write_bitstream) did not complete successfully"
}

open_run impl_1
write_hw_platform -fixed -include_bit -force -file $out_xsa

puts "run_step2_vivado.tcl: DONE. XSA written to $out_xsa"
