#include "feature_extract.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

#include "fixed_point.h"
#include "hann128_q15.h"

namespace zmpio::step1 {
namespace {

std::uint32_t vector_magnitude(const AxisSample &sample)
{
#ifdef __SYNTHESIS__
#pragma HLS ALLOCATION operation instances = mul limit = 1
#endif
    const std::int64_t x = sample.x;
    const std::int64_t y = sample.y;
    const std::int64_t z = sample.z;
    return integer_sqrt_u64(static_cast<std::uint64_t>(x * x + y * y + z * z));
}

std::uint32_t absolute_i32(std::int32_t value)
{
    return value < 0 ? static_cast<std::uint32_t>(-static_cast<std::int64_t>(value))
                     : static_cast<std::uint32_t>(value);
}

std::uint32_t kurtosis_q16_16(const std::array<std::int32_t, kFeatureWindow> &values,
                              std::uint32_t maximum)
{
    if (maximum == 0U) {
        return 0U;
    }

#ifdef __SYNTHESIS__
#pragma HLS ALLOCATION operation instances = mul limit = 1
#endif
    unsigned shift = 0U;
    while ((maximum >> shift) > 127U) {
        ++shift;
    }

    std::uint64_t sum2 = 0U;
    std::uint64_t sum4 = 0U;
    for (const std::int32_t value : values) {
        const std::uint32_t magnitude = absolute_i32(value) >> shift;
        const std::uint64_t square = static_cast<std::uint64_t>(magnitude) * magnitude;
        sum2 += square;
        sum4 += square * square;
    }
    if (sum2 == 0U) {
        return 0U;
    }

    const std::uint64_t denominator = sum2 * sum2;
    const std::uint64_t numerator =
        sum4 * static_cast<std::uint64_t>(kFeatureWindow) * 65536ULL;
    return saturate_u32((numerator + (denominator / 2U)) / denominator);
}

std::uint32_t frequency_q16_16(std::size_t bin)
{
    const std::uint64_t numerator =
        static_cast<std::uint64_t>(bin) * kSampleRateHz * 65536ULL;
    return static_cast<std::uint32_t>((numerator + (kFftSize / 2U)) / kFftSize);
}

}  // namespace

FeatureExtractor::FeatureExtractor()
{
    reset();
}

void FeatureExtractor::reset()
{
    magnitude_ring_.fill(0);
    write_index_ = 0U;
    samples_seen_ = 0U;
    frame_sequence_ = 0U;
}

bool FeatureExtractor::push(const AxisSample &filtered_sample,
                            std::uint32_t sample_sequence,
                            FeatureFrameV2 &frame)
{
    magnitude_ring_[write_index_] = static_cast<std::int32_t>(
        vector_magnitude(filtered_sample));
    write_index_ = (write_index_ + 1U) % kFeatureWindow;
    ++samples_seen_;

    if (samples_seen_ < kFeatureWindow ||
        ((samples_seen_ - kFeatureWindow) % kFeatureHop) != 0U) {
        return false;
    }

    build_frame(sample_sequence, frame);
    return true;
}

void FeatureExtractor::build_frame(std::uint32_t sample_sequence,
                                   FeatureFrameV2 &frame)
{
#ifdef __SYNTHESIS__
#pragma HLS ALLOCATION operation instances = mul limit = 1
#endif
    std::array<std::int32_t, kFeatureWindow> detrended{};
    std::int64_t sum = 0;
    for (std::size_t index = 0; index < kFeatureWindow; ++index) {
        sum += magnitude_ring_[(write_index_ + index) % kFeatureWindow];
    }
    const std::int32_t mean = static_cast<std::int32_t>(
        sum / static_cast<std::int64_t>(kFeatureWindow));

    std::uint64_t sum_square = 0U;
    std::uint32_t peak = 0U;
    RealFftInput fft_input{};
    for (std::size_t index = 0; index < kFeatureWindow; ++index) {
        const std::int32_t value =
            magnitude_ring_[(write_index_ + index) % kFeatureWindow] - mean;
        detrended[index] = value;
        peak = std::max(peak, absolute_i32(value));
        sum_square += static_cast<std::uint64_t>(
            static_cast<std::int64_t>(value) * value);
        fft_input[index] = round_shift_q15(
            static_cast<std::int64_t>(value) * kHannQ15[index]);
    }

    const std::uint64_t mean_square =
        sum_square / static_cast<std::uint64_t>(kFeatureWindow);
    const std::uint32_t rms = integer_sqrt_u64(mean_square);

    ComplexFftOutput spectrum{};
    fft128_real(fft_input, spectrum);

    std::array<std::uint64_t, 4> band_energy{};
    std::uint32_t dominant_power = 0U;
    std::size_t dominant_bin = 1U;
    for (std::size_t bin = 1U; bin <= 25U; ++bin) {
        const std::uint32_t power = fft_power_saturated(spectrum[bin]);
        if (power > dominant_power) {
            dominant_power = power;
            dominant_bin = bin;
        }
        const std::size_t band = (bin - 1U) / 6U;
        band_energy[std::min<std::size_t>(band, 3U)] += power;
    }

    frame.frame_sequence = frame_sequence_++;
    frame.window_end_sample_sequence = sample_sequence;
    frame.rms_q24_8 = saturate_u32(static_cast<std::uint64_t>(rms) << 8U);
    frame.peak_q24_8 = saturate_u32(static_cast<std::uint64_t>(peak) << 8U);
    frame.variance_q32_0 = saturate_u32(mean_square);
    frame.kurtosis_q16_16 = kurtosis_q16_16(detrended, peak);
    frame.dominant_frequency_q16_16 =
        dominant_power == 0U ? 0U : frequency_q16_16(dominant_bin);
    frame.dominant_power_q32_0 = dominant_power;
    for (std::size_t band = 0U; band < band_energy.size(); ++band) {
        frame.band_energy_q32_0[band] = saturate_u32(band_energy[band]);
    }
}

}  // namespace zmpio::step1
