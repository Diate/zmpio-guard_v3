#include "dsp_core_hls_top.h"

#include "dsp_core.h"

namespace zmpio::step1 {

void dsp_core_hls_top(const SampleFrameV1 &sample_in,
                       bool sample_in_valid,
                       FeatureFrameV2 &feature_out,
                       bool &feature_out_valid)
{
#pragma HLS INTERFACE ap_none port = sample_in
#pragma HLS INTERFACE ap_none port = sample_in_valid
#pragma HLS INTERFACE ap_none port = feature_out
#pragma HLS INTERFACE ap_none port = feature_out_valid
#pragma HLS INTERFACE ap_ctrl_hs port = return

    static DspCore core;

    feature_out_valid = false;
    feature_out = FeatureFrameV2{};

    if (sample_in_valid) {
        core.push(sample_in);
    }

    FeatureFrameV2 frame{};
    if (core.pop(frame)) {
        feature_out = frame;
        feature_out_valid = true;
    }
}

}  // namespace zmpio::step1
