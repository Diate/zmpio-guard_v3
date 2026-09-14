# arm_and_capture_ila.tcl
# Connects to the already-running board over JTAG (does NOT reprogram it - the
# app is already running from the current Vitis debug session), attaches the
# debug probes (.ltx) to detect the existing ila_0 core, arms a trigger on
# MDC (probe3) toggling, and waits for the user to restart the CPU0 app from
# Vitis so a fresh MDIO scan happens. Once triggered, dumps all 5 probes to a
# CSV file for analysis.

# Repository paths -- resolved from this script's own location.
source [file join [file dirname [info script]] .. .. rtl bd project_paths.tcl]
set probes_file [file join $zmpio(vivado_proj_dir) project_hub.runs impl_1 design_1_wrapper.ltx]
set out_csv     [file join $zmpio(vivado_proj_dir) ila_capture.csv]

open_hw_manager
connect_hw_server -allow_non_jtag
set target [lindex [get_hw_targets -quiet] 0]
if { $target eq "" } { error "No hw_target found" }
open_hw_target $target

set dev [lindex [get_hw_devices -quiet xc7z020_1] 0]
if { $dev eq "" } { error "xc7z020_1 device not found" }
current_hw_device $dev

set_property PROBES.FILE $probes_file $dev
refresh_hw_device $dev

set ila [lindex [get_hw_ilas -quiet -of_objects $dev] 0]
if { $ila eq "" } { error "No hw_ila core found on device - is the correct bitstream currently loaded?" }
puts "Found ILA: $ila"

set probes [get_hw_probes -quiet -of_objects $ila]
puts "Probes: $probes"

# Trigger on probe3 (MDC) rising edge - it toggles repeatedly during the MDIO
# scan, so we don't need to time this against the reset-release edge.
set mdc_probe [get_hw_probes -quiet -of_objects $ila *probe3*]
if { $mdc_probe eq "" } {
    puts "WARNING: probe3 not found by name, listing all probes:"
    foreach p $probes { puts "  $p" }
    error "Could not find probe3 (MDC) to set trigger on"
}
puts "Trigger probe: $mdc_probe"

set_property CONTROL.TRIGGER_POSITION 100 $ila
set_property TRIGGER_COMPARE_VALUE eq_R $mdc_probe
run_hw_ila $ila

puts "===== ILA ARMED - waiting for trigger (MDC toggle). ====="
puts "===== Please restart/re-run the cpu0_application debug session in Vitis now. ====="

set max_wait_s 600
set waited 0
set status ""
while { $waited < $max_wait_s } {
    set status [get_property STATUS $ila]
    if { $status eq "Waiting For Trigger" || $status eq "Idle" } {
        after 2000
        incr waited 2
        if { $waited % 20 == 0 } { puts "... still waiting (${waited}s), status=$status" }
    } else {
        break
    }
}

puts "Final ILA status: $status (waited ${waited}s)"

if { $status ne "Full" && $status ne "Ready" && $status ne "Uploaded" } {
    puts "WARNING: ILA did not report a captured/full status. Attempting upload anyway."
}

wait_on_hw_ila -timeout 10 $ila
upload_hw_ila_data $ila

set data [current_hw_ila_data]
puts "Captured samples: [get_property SAMPLES $data]"

write_hw_ila_data -csv_file $out_csv $data -force
puts "Wrote CSV to $out_csv"

puts "DONE arm_and_capture_ila.tcl"
