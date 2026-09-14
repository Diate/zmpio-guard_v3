// Vitis HLS C-simulation testbench for dsp_core_axis_top.
//
// Drives the same 640-sample synthetic tone stream as
// dsp_host_sim/fpga/dsp_core/hls/tb_hls_top.cpp through the AXI4-Stream interface
// (packing/unpacking exactly as mmio_axis_bridge.v and zmpio_dsp_ctrl.v will
// on real hardware) and checks the 9 resulting feature frames bit-exactly
// against golden/golden_tone_features.csv. This proves the new AXIS bit
// layout round-trips correctly, not just that DspCore itself is unchanged
// (Step1's own gate already covers that).

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "dsp_core_axis_top.h"

using namespace zmpio::step2;

namespace {

SampleAxis make_sample_beat(std::uint32_t sequence)
{
    constexpr double pi = 3.14159265358979323846;
    const double time = static_cast<double>(sequence) / 100.0;

    const std::int16_t accel_x = static_cast<std::int16_t>(
        std::lround(300.0 * std::sin(2.0 * pi * 4.0 * time)));
    const std::int16_t accel_y = static_cast<std::int16_t>(
        std::lround(180.0 * std::sin(2.0 * pi * 7.0 * time)));
    const std::int16_t accel_z = static_cast<std::int16_t>(
        std::lround(4096.0 + 1000.0 * std::sin(2.0 * pi * 10.0 * time)));

    ap_uint<160> data = 0;
    data.range(15, 0) = (std::uint16_t)accel_x;
    data.range(31, 16) = (std::uint16_t)accel_y;
    data.range(47, 32) = (std::uint16_t)accel_z;
    data.range(63, 48) = 0;   // temperature
    data.range(79, 64) = 0;   // gyro_x
    data.range(95, 80) = 0;   // gyro_y
    data.range(111, 96) = 0;  // gyro_z
    data.range(127, 112) = 0; // flags
    data.range(159, 128) = sequence;

    SampleAxis beat{};
    beat.data = data;
    beat.keep = -1;
    beat.strb = -1;
    beat.user = 1;
    beat.last = 1;
    return beat;
}

struct GoldenRow {
    std::uint32_t frame_sequence;
    std::uint32_t window_end_sample_sequence;
    std::uint32_t rms_q24_8;
    std::uint32_t peak_q24_8;
    std::uint32_t variance_q32_0;
    std::uint32_t kurtosis_q16_16;
    std::uint32_t dominant_frequency_q16_16;
    std::uint32_t dominant_power_q32_0;
    std::uint32_t band_energy_q32_0[4];
};

// Copied verbatim from dsp_host_sim/golden/golden_tone_features.csv (same as
// dsp_host_sim/fpga/dsp_core/hls/tb_hls_top.cpp).
constexpr GoldenRow kGolden[9] = {
    {0, 127, 293376, 989696, 1315037, 440545, 665600, 60050, {3749, 26960, 68305, 17}},
    {1, 191, 182272, 258304, 507723, 97606, 665600, 59753, {10, 27134, 68164, 7}},
    {2, 255, 181760, 260352, 504273, 98523, 665600, 59449, {19, 27178, 67865, 6}},
    {3, 319, 180224, 261632, 496653, 99213, 665600, 59834, {25, 27283, 68356, 10}},
    {4, 383, 180224, 261888, 495767, 99283, 665600, 59537, {24, 27108, 67873, 12}},
    {5, 447, 181504, 260352, 503737, 98561, 665600, 59653, {17, 27176, 68036, 10}},
    {6, 511, 182528, 258304, 508861, 97636, 665600, 59753, {7, 27182, 68162, 10}},
    {7, 575, 181504, 260608, 503246, 98414, 665600, 59914, {17, 27146, 68221, 9}},
    {8, 639, 180224, 261632, 495925, 99181, 665600, 59545, {34, 26993, 67915, 7}},
};

bool word_matches(std::uint32_t actual, std::uint32_t expected, std::size_t frame_index,
                  const char *field)
{
    if (actual != expected) {
        std::cerr << "FAIL: frame " << frame_index << " field " << field
                  << " actual=" << actual << " expected=" << expected << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main()
{
    int failures = 0;
    std::size_t frame_count = 0U;
    hls::stream<SampleAxis> sample_axis_in("sample_axis_in");
    hls::stream<FeatureAxis> feature_axis_out("feature_axis_out");

    for (std::uint32_t sequence = 0U; sequence < 640U; ++sequence) {
        sample_axis_in.write(make_sample_beat(sequence));
        dsp_core_axis_top(sample_axis_in, feature_axis_out);

        if (feature_axis_out.empty()) {
            continue;
        }
        const FeatureAxis beat = feature_axis_out.read();
        if (frame_count >= 9U) {
            std::cerr << "FAIL: more than 9 feature frames produced\n";
            ++failures;
            break;
        }

        const GoldenRow &expected = kGolden[frame_count];
        const ap_uint<384> data = beat.data;
        bool ok = true;
        ok &= word_matches(data.range(31, 0), expected.frame_sequence, frame_count,
                           "frame_sequence");
        ok &= word_matches(data.range(63, 32), expected.window_end_sample_sequence,
                           frame_count, "window_end_sample_sequence");
        ok &= word_matches(data.range(95, 64), expected.rms_q24_8, frame_count, "rms_q24_8");
        ok &= word_matches(data.range(127, 96), expected.peak_q24_8, frame_count,
                           "peak_q24_8");
        ok &= word_matches(data.range(159, 128), expected.variance_q32_0, frame_count,
                           "variance_q32_0");
        ok &= word_matches(data.range(191, 160), expected.kurtosis_q16_16, frame_count,
                           "kurtosis_q16_16");
        ok &= word_matches(data.range(223, 192), expected.dominant_frequency_q16_16,
                           frame_count, "dominant_frequency_q16_16");
        ok &= word_matches(data.range(255, 224), expected.dominant_power_q32_0, frame_count,
                           "dominant_power_q32_0");
        ok &= word_matches(data.range(287, 256), expected.band_energy_q32_0[0], frame_count,
                           "band_energy_q32_0[0]");
        ok &= word_matches(data.range(319, 288), expected.band_energy_q32_0[1], frame_count,
                           "band_energy_q32_0[1]");
        ok &= word_matches(data.range(351, 320), expected.band_energy_q32_0[2], frame_count,
                           "band_energy_q32_0[2]");
        ok &= word_matches(data.range(383, 352), expected.band_energy_q32_0[3], frame_count,
                           "band_energy_q32_0[3]");
        if (!ok) {
            ++failures;
        }
        ++frame_count;
    }

    if (frame_count != 9U) {
        std::cerr << "FAIL: expected 9 feature frames, got " << frame_count << '\n';
        ++failures;
    }

    if (failures == 0) {
        std::cout << "AXIS HLS csim PASS: " << frame_count
                  << " feature frames bit-exact vs golden (via AXIS round-trip)\n";
        return EXIT_SUCCESS;
    }

    std::cerr << failures << " check(s) failed\n";
    return EXIT_FAILURE;
}
