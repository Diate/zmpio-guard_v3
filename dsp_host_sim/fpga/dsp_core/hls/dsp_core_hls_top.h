#ifndef ZMPIO_STEP1_DSP_CORE_HLS_TOP_H
#define ZMPIO_STEP1_DSP_CORE_HLS_TOP_H

#include "feature_frame_v2.h"
#include "sample_frame_v1.h"

namespace zmpio::step1 {

// Minimal Vitis HLS top-level wrapper around DspCore, used only to close the
// Step1 "HLS resource <=24 DSP48E1 / PL timing" gate (see
// dsp_host_sim/docs/IMPLEMENTATION_AND_DEPLOYMENT.md, section A.5).
// It is not the Step2 AXIS/AXI-Lite shell -- Step2 defines the real IP
// interfaces (mmio_axis_bridge, zmpio_dsp_ctrl) around this same DspCore.
void dsp_core_hls_top(const SampleFrameV1 &sample_in,
                       bool sample_in_valid,
                       FeatureFrameV2 &feature_out,
                       bool &feature_out_valid);

}  // namespace zmpio::step1

#endif
