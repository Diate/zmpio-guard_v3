#include "dsp_core_axis_top.h"

#include "dsp_core.h"

namespace zmpio::step2 {

namespace {

zmpio::step1::SampleFrameV1 unpack_sample(const SampleAxis &beat)
{
    zmpio::step1::SampleFrameV1 sample{};
    sample.accel_x = (std::int16_t)(std::uint16_t)beat.data.range(15, 0);
    sample.accel_y = (std::int16_t)(std::uint16_t)beat.data.range(31, 16);
    sample.accel_z = (std::int16_t)(std::uint16_t)beat.data.range(47, 32);
    sample.temperature = (std::int16_t)(std::uint16_t)beat.data.range(63, 48);
    sample.gyro_x = (std::int16_t)(std::uint16_t)beat.data.range(79, 64);
    sample.gyro_y = (std::int16_t)(std::uint16_t)beat.data.range(95, 80);
    sample.gyro_z = (std::int16_t)(std::uint16_t)beat.data.range(111, 96);
    sample.flags = (std::uint16_t)beat.data.range(127, 112);
    sample.sample_sequence = (std::uint32_t)beat.data.range(159, 128);
    return sample;
}

FeatureAxis pack_feature(const zmpio::step1::FeatureFrameV2 &frame)
{
    FeatureAxis beat{};
    ap_uint<384> data = 0;
    data.range(31, 0) = frame.frame_sequence;
    data.range(63, 32) = frame.window_end_sample_sequence;
    data.range(95, 64) = frame.rms_q24_8;
    data.range(127, 96) = frame.peak_q24_8;
    data.range(159, 128) = frame.variance_q32_0;
    data.range(191, 160) = frame.kurtosis_q16_16;
    data.range(223, 192) = frame.dominant_frequency_q16_16;
    data.range(255, 224) = frame.dominant_power_q32_0;
    data.range(287, 256) = frame.band_energy_q32_0[0];
    data.range(319, 288) = frame.band_energy_q32_0[1];
    data.range(351, 320) = frame.band_energy_q32_0[2];
    data.range(383, 352) = frame.band_energy_q32_0[3];
    beat.data = data;
    beat.keep = -1;
    beat.strb = -1;
    beat.user = 1;
    beat.last = 1;
    return beat;
}

}  // namespace

void dsp_core_axis_top(hls::stream<SampleAxis> &sample_axis_in,
                        hls::stream<FeatureAxis> &feature_axis_out)
{
#pragma HLS INTERFACE axis port = sample_axis_in
#pragma HLS INTERFACE axis port = feature_axis_out
#pragma HLS INTERFACE ap_ctrl_none port = return

    // Free-running: no s_axilite control on this block. RUN/STATUS/
    // FEATURE_COUNT/DROP_COUNT/CONFIG_SEQ live in zmpio_dsp_ctrl.v, the
    // separate AXI4-Lite peripheral downstream of feature_axis_out (see
    // docs/sdd_sad/SDD_10_Step2_PL_Shell.md) -- this core only ever
    // transforms sample beats into feature beats.
    static zmpio::step1::DspCore core;

    if (!sample_axis_in.empty()) {
        const SampleAxis beat = sample_axis_in.read();
        core.push(unpack_sample(beat));
    }

    zmpio::step1::FeatureFrameV2 frame{};
    if (core.pop(frame)) {
        feature_axis_out.write(pack_feature(frame));
    }
}

}  // namespace zmpio::step2
