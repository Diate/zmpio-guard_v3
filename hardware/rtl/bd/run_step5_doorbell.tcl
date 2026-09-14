# Driver script for Step 5 (docs/ROADMAP.md STEP 5, docs/sdd_sad/
# SDD_DOORBELL.md): opens project_hub, applies add_zmpio_doorbell.tcl,
# rebuilds the bitstream, exports the hardware platform. Run with:
#   vivado -mode batch -source hardware/rtl/bd/run_step5_doorbell.tcl
#
# Mirrors run_step3_dsp_soft_reset.tcl's shape (not run_step2_vivado.tcl's --
# that one's extra top-wrapper/AutoDisabled/xlconcat-recreate fixups repair a
# broken BD state that does not apply here; add_zmpio_doorbell.tcl already
# does its own xlconcat_0 delete+recreate for the reason explained in its own
# comments, so this driver does not need to duplicate that here).
#
# Prerequisite: hardware/rtl/bd/package_zmpio_user_ip.tcl already re-run
# standalone (no project open) against the current zmpio_doorbell.v, so
# hardware/rtl/ip_repo/zmpio_doorbell/component.xml exists before this
# script's update_ip_catalog (inside add_zmpio_doorbell.tcl) runs.
#
# Take your own backup of hardware/vivado/project_hub before running this --
# same caution as run_step2_vivado.tcl/run_step3_dsp_soft_reset.tcl.

# Repository paths -- resolved from this script's own location.
source [file join [file dirname [info script]] project_paths.tcl]
set project_xpr $zmpio(project_xpr)
set out_xsa $zmpio(out_xsa)

open_project $project_xpr

set bd_files [get_files -quiet design_1.bd]
if {[llength $bd_files] == 0} {
    error "run_step5_doorbell.tcl: design_1.bd not found in project_hub.xpr."
}
open_bd_design [lindex $bd_files 0]

# add_zmpio_doorbell.tcl mutates design_1.bd on disk (create_bd_cell +
# save_bd_design) -- re-sourcing it against a design_1.bd that already has
# zmpio_doorbell_0 (e.g. re-running this driver after a later step failed)
# would error out trying to create a cell that already exists. Only apply it
# once.
if {[llength [get_bd_cells -quiet zmpio_doorbell_0]] == 0} {
    source [file join $zmpio(bd_dir) add_zmpio_doorbell.tcl]
} else {
    puts "run_step5_doorbell.tcl: design_1.bd already has zmpio_doorbell_0 -- skipping add_zmpio_doorbell.tcl."
}

update_compile_order -fileset sources_1
generate_target all [get_files design_1.bd] -force

# See run_step2_vivado.tcl/run_step3_dsp_soft_reset.tcl for why this is
# needed unconditionally on this project (stale AUTO_INCREMENTAL_CHECKPOINT
# pointing at an unrelated project's .dcp).
set_property AUTO_INCREMENTAL_CHECKPOINT 0 [get_runs synth_1]

reset_run synth_1
launch_runs synth_1 -jobs 4
wait_on_run synth_1
if {[get_property PROGRESS [get_runs synth_1]] != "100%"} {
    error "run_step5_doorbell.tcl: synth_1 did not complete successfully"
}

# See run_step2_vivado.tcl for why write_bitstream needs this DRC suppression
# (gmii_to_mii OOC module has no package pins of its own).
set_property STEPS.WRITE_BITSTREAM.TCL.PRE \
    [file join $zmpio(bd_dir) suppress_ooc_io_drc.tcl] [get_runs impl_1]

reset_run impl_1
launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1
if {[get_property PROGRESS [get_runs impl_1]] != "100%"} {
    error "run_step5_doorbell.tcl: impl_1 (write_bitstream) did not complete successfully"
}

open_run impl_1
write_hw_platform -fixed -include_bit -force -file $out_xsa

puts "run_step5_doorbell.tcl: DONE. XSA written to $out_xsa"
