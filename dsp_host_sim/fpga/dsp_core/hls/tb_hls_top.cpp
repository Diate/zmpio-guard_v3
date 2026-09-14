// Vitis HLS C-simulation testbench for dsp_core_hls_top.
//
// Replays the same 640-sample synthetic tone stream used by
// fpga/dsp_core/tb/gen_golden.cpp and checks the 9 resulting feature frames
// bit-exactly against golden/golden_tone_features.csv (values copied below
// so csim does not depend on a working-directory-relative file path).

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "dsp_core_hls_top.h"

using namespace zmpio::step1;

namespace {

SampleFrameV1 make_sample(std::uint32_t sequence)
{
    constexpr double pi = 3.14159265358979323846;
    const double time = static_cast<double>(sequence) / 100.0;
    SampleFrameV1 sample{};
    sample.accel_x = static_cast<std::int16_t>(
        std::lround(300.0 * std::sin(2.0 * pi * 4.0 * time)));
    sample.accel_y = static_cast<std::int16_t>(
        std::lround(180.0 * std::sin(2.0 * pi * 7.0 * time)));
    sample.accel_z = static_cast<std::int16_t>(
        std::lround(4096.0 + 1000.0 * std::sin(2.0 * pi * 10.0 * time)));
    sample.temperature = 0;
    sample.gyro_x = 0;
    sample.gyro_y = 0;
    sample.gyro_z = 0;
    sample.flags = 0U;
    sample.sample_sequence = sequence;
    return sample;
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

// Copied verbatim from dsp_host_sim/golden/golden_tone_features.csv.
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

}  // namespace

int main()
{
    int failures = 0;
    std::size_t frame_count = 0U;

    for (std::uint32_t sequence = 0U; sequence < 640U; ++sequence) {
        const SampleFrameV1 sample = make_sample(sequence);
        FeatureFrameV2 feature{};
        bool feature_valid = false;

        dsp_core_hls_top(sample, true, feature, feature_valid);

        if (!feature_valid) {
            continue;
        }
        if (frame_count >= 9U) {
            std::cerr << "FAIL: more than 9 feature frames produced\n";
            ++failures;
            break;
        }

        const GoldenRow &expected = kGolden[frame_count];
        if (feature.frame_sequence != expected.frame_sequence ||
            feature.window_end_sample_sequence !=
                expected.window_end_sample_sequence ||
            feature.rms_q24_8 != expected.rms_q24_8 ||
            feature.peak_q24_8 != expected.peak_q24_8 ||
            feature.variance_q32_0 != expected.variance_q32_0 ||
            feature.kurtosis_q16_16 != expected.kurtosis_q16_16 ||
            feature.dominant_frequency_q16_16 !=
                expected.dominant_frequency_q16_16 ||
            feature.dominant_power_q32_0 != expected.dominant_power_q32_0 ||
            feature.band_energy_q32_0[0] != expected.band_energy_q32_0[0] ||
            feature.band_energy_q32_0[1] != expected.band_energy_q32_0[1] ||
            feature.band_energy_q32_0[2] != expected.band_energy_q32_0[2] ||
            feature.band_energy_q32_0[3] != expected.band_energy_q32_0[3]) {
            std::cerr << "FAIL: frame " << frame_count
                      << " does not match golden vector\n";
            ++failures;
        }
        ++frame_count;
    }

    if (frame_count != 9U) {
        std::cerr << "FAIL: expected 9 feature frames, got " << frame_count
                   << '\n';
        ++failures;
    }

    if (failures == 0) {
        std::cout << "HLS csim PASS: " << frame_count
                   << " feature frames bit-exact vs golden\n";
        return EXIT_SUCCESS;
    }

    std::cerr << failures << " check(s) failed\n";
    return EXIT_FAILURE;
}
