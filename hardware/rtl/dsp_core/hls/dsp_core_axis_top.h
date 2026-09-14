#ifndef ZMPIO_STEP2_DSP_CORE_AXIS_TOP_H
#define ZMPIO_STEP2_DSP_CORE_AXIS_TOP_H

#include "ap_axi_sdata.h"
#include "hls_stream.h"

namespace zmpio::step2 {

// AXI4-Stream beat carrying one Step1 SampleFrameV1 (160 bits = 20 bytes).
// Bit layout mirrors mmio_axis_bridge.v's SAMPLE_W0..W4 registers exactly
// (see ROADMAP_DETAIL.md Step 2 / docs/sdd_sad/SDD_10_Step2_PL_Shell.md):
// tdata[15:0]=accel_x, [31:16]=accel_y, [47:32]=accel_z, [63:48]=temperature,
// [79:64]=gyro_x, [95:80]=gyro_y, [111:96]=gyro_z, [127:112]=flags,
// [159:128]=sample_sequence.
using SampleAxis = ap_axiu<160, 1, 1, 1>;

// AXI4-Stream beat carrying one Step1 FeatureFrameV2 (384 bits = 48 bytes).
// tdata[31:0]=frame_sequence, [63:32]=window_end_sample_sequence,
// [95:64]=rms_q24_8, [127:96]=peak_q24_8, [159:128]=variance_q32_0,
// [191:160]=kurtosis_q16_16, [223:192]=dominant_frequency_q16_16,
// [255:224]=dominant_power_q32_0, [287:256..383:352]=band_energy_q32_0[0..3].
using FeatureAxis = ap_axiu<384, 1, 1, 1>;

// Free-running AXIS-in/AXIS-out shell around the same zmpio::step1::DspCore
// that Step1 proved bit-exact and HLS-closed at 25 DSP48E1 / 50 MHz
// (dsp_host_sim/docs/IMPLEMENTATION_AND_DEPLOYMENT.md). Step1's dsp_core_hls_top
// (ap_none ports) stays untouched -- this is a new, separate top-level with the
// real streaming interfaces Step2's PL shell needs; see
// dsp_host_sim/fpga/dsp_core/hls/dsp_core_hls_top.h.
void dsp_core_axis_top(hls::stream<SampleAxis> &sample_axis_in,
                        hls::stream<FeatureAxis> &feature_axis_out);

}  // namespace zmpio::step2

#endif
