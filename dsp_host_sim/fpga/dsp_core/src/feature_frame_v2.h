#ifndef ZMPIO_STEP1_FEATURE_FRAME_V2_H
#define ZMPIO_STEP1_FEATURE_FRAME_V2_H

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace zmpio::step1 {

struct FeatureFrameV2 {
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

static_assert(sizeof(FeatureFrameV2) == 48U,
              "feature_frame_v2 must be exactly 48 bytes");
static_assert(offsetof(FeatureFrameV2, band_energy_q32_0) == 32U,
              "feature band offset changed");
static_assert(std::is_standard_layout<FeatureFrameV2>::value,
              "feature frame must have a stable C ABI layout");
static_assert(std::is_trivially_copyable<FeatureFrameV2>::value,
              "feature frame must remain trivially copyable");

}  // namespace zmpio::step1

#endif

