# Vitis HLS batch script to close the Step1 gates left OPEN in
# dsp_host_sim/docs/IMPLEMENTATION_AND_DEPLOYMENT.md (section A.5):
# "Vitis HLS synthesis/resource <=24 DSP48E1" and "Timing at final PL clock".
#
# Usage (from this directory or anywhere, paths below are relative to this
# script's location via [file dirname [info script]]):
#   vitis_hls -f run_hls.tcl
#
# Part: xc7z020clg400-2 (Z7-Lite board, see docs/architecture/02_VIVADO_HARDWARE.md)
# Clock: 50 MHz (20 ns period) -- matches the existing FCLK0 (~50 MHz) already
# proven on this board for axi_iic_0 (docs/architecture/03_CLOCK_ARCHITECTURE.md).
# A first pass at 100 MHz (10 ns) failed timing once enough resource-sharing
# pragmas were added to hit the <=24 DSP48E1 budget; ROADMAP_DETAIL.md only
# lists 100 MHz as a tentative default ("thuong 100 MHz hoac clock PS"), not a
# hard requirement, so reusing the already-proven FCLK0 rate is preferred over
# adding a second, unproven FCLK for Step 2.

set script_dir [file dirname [info script]]
set src_dir [file normalize [file join $script_dir .. src]]
set hls_dir [file normalize $script_dir]

open_project -reset dsp_core_hls_proj
set_top dsp_core_hls_top

add_files $hls_dir/dsp_core_hls_top.cpp -cflags "-I $src_dir -I $hls_dir -std=c++14"
add_files $src_dir/dsp_core.cpp -cflags "-I $src_dir -std=c++14"
add_files $src_dir/fir3_axis.cpp -cflags "-I $src_dir -std=c++14"
add_files $src_dir/feature_extract.cpp -cflags "-I $src_dir -std=c++14"
add_files $src_dir/fft128_real.cpp -cflags "-I $src_dir -std=c++14"

add_files -tb $hls_dir/tb_hls_top.cpp -cflags "-I $src_dir -I $hls_dir -std=c++14"

open_solution -reset "solution1"
set_part {xc7z020clg400-2}
create_clock -period 20 -name default

# Vitis HLS (unlike classic Vivado HLS) auto-pipelines small-trip-count loops
# by default, which forces full array partitioning and fully unrolls the
# 16-tap x3-axis FIR and FFT butterfly stages -- that is what drove the first
# run to 69 DSP48E1 (vs the <=24 budget) and a timing violation. Disable that
# auto-pipelining so loops stay rolled (time-multiplexed) unless a loop is
# explicitly pragma'd.
config_compile -pipeline_loops 0

csim_design -clean
csynth_design

close_project
exit
