# Vitis HLS batch script for Step2's dsp_core_axis_top -- the AXIS/AXI-Lite
# shell around the same DspCore Step1 already closed at 25 DSP48E1 / 50 MHz
# (dsp_host_sim/docs/IMPLEMENTATION_AND_DEPLOYMENT.md). Mirrors
# dsp_host_sim/fpga/dsp_core/hls/run_hls.tcl; see that
# file and docs/sdd_sad/SDD_09_Step1_DSP_Core_TinyML.md Sec.12.8 for the
# Windows PATH/make gotchas (MinGW gcc ahead of MSYS in PATH, /dev/null
# collision, PowerShell pipe truncation) that block this script if not
# worked around first.
#
# Usage (from this directory or anywhere, paths below are relative to this
# script's location via [file dirname [info script]]):
#   vitis_hls -f run_hls_axis.tcl
#
# Part: xc7z020clg400-2 (Z7-Lite board). Clock: 50 MHz (20 ns period), same
# FCLK0 the DSP core is already timing-closed against.

set script_dir [file dirname [info script]]
# script_dir is hardware/rtl/dsp_core/hls/; four levels up reaches the repo
# root that holds dsp_host_sim/ (three levels up only reaches hardware/).
set step1_src_dir [file normalize [file join $script_dir .. .. .. .. dsp_host_sim fpga dsp_core src]]
set hls_dir [file normalize $script_dir]

open_project -reset dsp_core_axis_hls_proj
set_top dsp_core_axis_top

add_files $hls_dir/dsp_core_axis_top.cpp -cflags "-I $step1_src_dir -I $hls_dir -std=c++14"
add_files $step1_src_dir/dsp_core.cpp -cflags "-I $step1_src_dir -std=c++14"
add_files $step1_src_dir/fir3_axis.cpp -cflags "-I $step1_src_dir -std=c++14"
add_files $step1_src_dir/feature_extract.cpp -cflags "-I $step1_src_dir -std=c++14"
add_files $step1_src_dir/fft128_real.cpp -cflags "-I $step1_src_dir -std=c++14"

add_files -tb $hls_dir/tb_dsp_core_axis_top.cpp -cflags "-I $step1_src_dir -I $hls_dir -std=c++14"

open_solution -reset "solution1"
set_part {xc7z020clg400-2}
create_clock -period 20 -name default

# Same rationale as dsp_host_sim/fpga/dsp_core/hls/run_hls.tcl: Vitis HLS
# auto-pipelines small-trip-count loops by default, which blew the Step1
# resource budget until disabled.
config_compile -pipeline_loops 0

csim_design -clean
csynth_design

# Package as a Vivado-catalog IP so hardware/rtl/bd/add_zmpio_dsp_shell.tcl can pull
# it into project_hub_step2 with add_files -norecurse / update_ip_catalog,
# same as any other packaged HLS core.
export_design -rtl verilog -format ip_catalog -display_name "ZMPIO DSP Core AXIS (Step2)" \
    -description "AXIS/AXI4-Lite shell around the Step1-proven FIR/FFT/feature DspCore" \
    -vendor "zmpio" -library "dsp_core" -version "1.0"

# Auto-copy exported IP to permanent, git-tracked location
set hls_ip_export "$hls_dir/dsp_core_axis_hls_proj/solution1/impl/ip"
set ip_repo_dest [file normalize [file join $script_dir .. .. .. hardware rtl ip_repo dsp_core_axis_top]]

if {[file exists $hls_ip_export]} {
    puts "Copying HLS IP from $hls_ip_export to $ip_repo_dest..."

    # Remove old destination if it exists
    if {[file exists $ip_repo_dest]} {
        file delete -force $ip_repo_dest
    }
    file mkdir $ip_repo_dest

    # Copy core IP files (skip transient HLS artifacts)
    foreach item {component.xml hdl xgui doc constraints misc subcore} {
        set src [file join $hls_ip_export $item]
        if {[file exists $src]} {
            file copy -force $src [file join $ip_repo_dest $item]
            puts "  copied $item"
        }
    }
    puts "HLS IP auto-copy completed to $ip_repo_dest"
} else {
    puts "WARNING: HLS IP export not found at $hls_ip_export"
}

close_project
exit
