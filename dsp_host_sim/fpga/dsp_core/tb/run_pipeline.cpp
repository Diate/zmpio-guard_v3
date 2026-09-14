#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "dsp_core.h"
#include "sample_frame_v1.h"
#include "feature_frame_v2.h"

namespace {

using namespace zmpio::step1;

struct CsvRow {
    std::string session_id;
    std::string label;
    std::uint32_t sequence;
    std::uint32_t timestamp_us;
    std::uint32_t flags;
    std::int16_t accel_x;
    std::int16_t accel_y;
    std::int16_t accel_z;
    std::int16_t temperature;
    std::int16_t gyro_x;
    std::int16_t gyro_y;
    std::int16_t gyro_z;
};

bool parse_csv_row(const std::string &line, CsvRow &row)
{
    std::istringstream stream(line);
    std::string token;
    std::vector<std::string> tokens;
    while (std::getline(stream, token, ',')) {
        tokens.push_back(token);
    }
    if (tokens.size() < 12) {
        return false;
    }
    row.session_id     = tokens[0];
    row.label          = tokens[1];
    row.sequence       = static_cast<std::uint32_t>(std::stoul(tokens[2]));
    row.timestamp_us   = static_cast<std::uint32_t>(std::stoul(tokens[3]));
    row.flags          = static_cast<std::uint32_t>(std::stoul(tokens[4]));
    row.accel_x        = static_cast<std::int16_t>(std::stoi(tokens[5]));
    row.accel_y        = static_cast<std::int16_t>(std::stoi(tokens[6]));
    row.accel_z        = static_cast<std::int16_t>(std::stoi(tokens[7]));
    row.temperature    = static_cast<std::int16_t>(std::stoi(tokens[8]));
    row.gyro_x         = static_cast<std::int16_t>(std::stoi(tokens[9]));
    row.gyro_y         = static_cast<std::int16_t>(std::stoi(tokens[10]));
    row.gyro_z         = static_cast<std::int16_t>(std::stoi(tokens[11]));
    return true;
}

SampleFrameV1 to_sample(const CsvRow &row)
{
    SampleFrameV1 sample{};
    sample.accel_x        = row.accel_x;
    sample.accel_y        = row.accel_y;
    sample.accel_z        = row.accel_z;
    sample.temperature    = row.temperature;
    sample.gyro_x         = row.gyro_x;
    sample.gyro_y         = row.gyro_y;
    sample.gyro_z         = row.gyro_z;
    sample.flags          = row.flags;
    sample.sample_sequence = row.sequence;
    return sample;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::cerr << "Usage: run_pipeline <raw_csv> <feature_csv>\n";
        return EXIT_FAILURE;
    }

    const std::string input_path(argv[1]);
    const std::string output_path(argv[2]);

    std::ifstream input(input_path.c_str());
    if (!input) {
        std::cerr << "Cannot open input: " << input_path << '\n';
        return EXIT_FAILURE;
    }

    std::ofstream output(output_path.c_str(), std::ios::trunc);
    if (!output) {
        std::cerr << "Cannot open output: " << output_path << '\n';
        return EXIT_FAILURE;
    }

    output << "session_id,label,frame_sequence,window_end_sample_sequence,"
              "rms_q24_8,peak_q24_8,variance_q32_0,kurtosis_q16_16,"
              "dominant_frequency_q16_16,dominant_power_q32_0,"
              "band0_q32_0,band1_q32_0,band2_q32_0,band3_q32_0\n";

    DspCore core;
    std::string session_id;
    std::string label;
    std::uint64_t sample_count = 0U;
    std::uint64_t feature_count = 0U;

    std::string line;
    // skip header
    if (!std::getline(input, line)) {
        std::cerr << "Empty input file\n";
        return EXIT_FAILURE;
    }

    while (std::getline(input, line)) {
        CsvRow row{};
        if (!parse_csv_row(line, row)) {
            std::cerr << "Failed to parse line: " << line.substr(0, 60) << "...\n";
            continue;
        }
        session_id = row.session_id;
        label = row.label;

        // Feed until accepted (back-pressure handling)
        while (!core.push(to_sample(row))) {
            // Consume pending output first
            if (core.output_valid()) {
                FeatureFrameV2 frame{};
                if (!core.pop(frame)) {
                    return EXIT_FAILURE;
                }
                output << session_id << ',' << label << ','
                       << frame.frame_sequence << ','
                       << frame.window_end_sample_sequence << ','
                       << frame.rms_q24_8 << ',' << frame.peak_q24_8 << ','
                       << frame.variance_q32_0 << ',' << frame.kurtosis_q16_16 << ','
                       << frame.dominant_frequency_q16_16 << ','
                       << frame.dominant_power_q32_0;
                for (const auto energy : frame.band_energy_q32_0) {
                    output << ',' << energy;
                }
                output << '\n';
                ++feature_count;
            }
        }
        ++sample_count;

        // Drain any ready output
        while (core.output_valid()) {
            FeatureFrameV2 frame{};
            if (!core.pop(frame)) {
                return EXIT_FAILURE;
            }
            output << session_id << ',' << label << ','
                   << frame.frame_sequence << ','
                   << frame.window_end_sample_sequence << ','
                   << frame.rms_q24_8 << ',' << frame.peak_q24_8 << ','
                   << frame.variance_q32_0 << ',' << frame.kurtosis_q16_16 << ','
                   << frame.dominant_frequency_q16_16 << ','
                   << frame.dominant_power_q32_0;
            for (const auto energy : frame.band_energy_q32_0) {
                output << ',' << energy;
            }
            output << '\n';
            ++feature_count;
        }
    }

    std::cout << "Pipeline complete: " << sample_count << " samples -> "
              << feature_count << " feature frames\n";
    return EXIT_SUCCESS;
}
