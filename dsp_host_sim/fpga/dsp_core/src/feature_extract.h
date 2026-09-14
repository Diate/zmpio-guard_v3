#ifndef ZMPIO_STEP1_FEATURE_EXTRACT_H
#define ZMPIO_STEP1_FEATURE_EXTRACT_H

#include <array>
#include <cstddef>
#include <cstdint>

#include "feature_frame_v2.h"
#include "fft128_real.h"
#include "sample_frame_v1.h"

namespace zmpio::step1 {

constexpr std::size_t kFeatureWindow = 128U;
constexpr std::size_t kFeatureHop = 64U;
constexpr std::uint32_t kSampleRateHz = 100U;

class FeatureExtractor {
public:
    FeatureExtractor();

    void reset();
    bool push(const AxisSample &filtered_sample,
              std::uint32_t sample_sequence,
              FeatureFrameV2 &frame);

private:
    void build_frame(std::uint32_t sample_sequence, FeatureFrameV2 &frame);

    std::array<std::int32_t, kFeatureWindow> magnitude_ring_{};
    std::size_t write_index_ = 0U;
    std::uint64_t samples_seen_ = 0U;
    std::uint32_t frame_sequence_ = 0U;
};

}  // namespace zmpio::step1

#endif

