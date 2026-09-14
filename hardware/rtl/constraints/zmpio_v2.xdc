# Step2 PL shell constraints (docs/ROADMAP_DETAIL.md Buoc 2).
#
# mmio_axis_bridge and zmpio_dsp_ctrl are internal AXI4-Lite/AXI4-Stream
# peripherals on the existing FCLK0 (~50 MHz) domain -- no new package pins,
# no new clock. dsp_core_axis_top reuses the same FCLK0 timing already
# closed for the DSP core in Step1 (dsp_host_sim/docs/IMPLEMENTATION_AND_DEPLOYMENT.md, +0.19 ns slack at
# 25 DSP48E1) and re-confirmed for this AXIS wrapper (hardware/rtl/dsp_core/hls,
# 2026-08-22 csynth: 25 DSP48E1/11%, 33 BRAM/11%, Fmax est. 69.41 MHz).
#
# Intentionally empty otherwise. If a future revision adds an external pin
# (e.g. a debug/status GPIO), add it here with the same PACKAGE_PIN/
# IOSTANDARD documentation style as the project constraints under
# hardware/vivado/project_hub.
