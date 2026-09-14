#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include "dsp_core.h"

namespace {

using namespace zmpio::step1;

SampleFrameV1 make_sample(std::uint32_t sequence)
{
    constexpr double pi = 3.14159265358979323846;
    const double time = static_cast<double>(sequence) / 100.0;
    SampleFrameV1 sample{};
    sample.accel_x = static_cast<std::int16_t>(
        std::lround(300.0 * std::sin(2.0 * pi * 4.0 * time)));
    sample.accel_y = static_cast<std::int16_t>(
        std::lround(180.0 * std::sin(2.0 * pi * 7.0 * time)));
    sample.accel_z = static_cast<std::int16_t>(std::lround(
        4096.0 + 1000.0 * std::sin(2.0 * pi * 10.0 * time)));
    sample.temperature = 0;
    sample.gyro_x = 0;
    sample.gyro_y = 0;
    sample.gyro_z = 0;
    sample.flags = 0U;
    sample.sample_sequence = sequence;
    return sample;
}

}  // namespace

int main(int argc, char **argv)
{
    const std::string output_path =
        argc > 1 ? std::string(argv[1]) : std::string("golden_tone_features.csv");
    std::ofstream output(output_path.c_str(), std::ios::trunc);
    if (!output) {
        std::cerr << "Cannot open golden output: " << output_path << '\n';
        return EXIT_FAILURE;
    }

    output << "frame_sequence,window_end_sample_sequence,rms_q24_8,peak_q24_8,"
              "variance_q32_0,kurtosis_q16_16,dominant_frequency_q16_16,"
              "dominant_power_q32_0,band0_q32_0,band1_q32_0,band2_q32_0,"
              "band3_q32_0\n";

    DspCore core;
    std::size_t frame_count = 0U;
    for (std::uint32_t sequence = 0U; sequence < 640U; ++sequence) {
        if (!core.push(make_sample(sequence))) {
            std::cerr << "Unexpected back-pressure while writing golden input\n";
            return EXIT_FAILURE;
        }
        if (core.output_valid()) {
            FeatureFrameV2 frame{};
            if (!core.pop(frame)) {
                return EXIT_FAILURE;
            }
            output << frame.frame_sequence << ','
                   << frame.window_end_sample_sequence << ','
                   << frame.rms_q24_8 << ',' << frame.peak_q24_8 << ','
                   << frame.variance_q32_0 << ',' << frame.kurtosis_q16_16 << ','
                   << frame.dominant_frequency_q16_16 << ','
                   << frame.dominant_power_q32_0;
            for (const std::uint32_t energy : frame.band_energy_q32_0) {
                output << ',' << energy;
            }
            output << '\n';
            ++frame_count;
        }
    }

    std::cout << "Wrote " << frame_count << " feature frames to " << output_path
              << '\n';
    return frame_count == 9U ? EXIT_SUCCESS : EXIT_FAILURE;
}
