#include "fft128_real.h"

#include <algorithm>
#include <cstdint>

#include "fft_twiddles_q15.h"
#include "fixed_point.h"

namespace zmpio::step1 {
namespace {

std::size_t reverse_seven_bits(std::size_t value)
{
    std::size_t reversed = 0U;
    for (unsigned bit = 0; bit < 7U; ++bit) {
        reversed = (reversed << 1U) | ((value >> bit) & 1U);
    }
    return reversed;
}

ComplexFixed multiply_twiddle(const ComplexFixed &value, std::size_t index)
{
#ifdef __SYNTHESIS__
#pragma HLS ALLOCATION operation instances = mul limit = 1
#endif
    const std::int64_t real =
        static_cast<std::int64_t>(value.real) * kTwiddleRealQ15[index] -
        static_cast<std::int64_t>(value.imag) * kTwiddleImagQ15[index];
    const std::int64_t imag =
        static_cast<std::int64_t>(value.real) * kTwiddleImagQ15[index] +
        static_cast<std::int64_t>(value.imag) * kTwiddleRealQ15[index];
    return {round_shift_q15(real), round_shift_q15(imag)};
}

}  // namespace

void fft128_real(const RealFftInput &input, ComplexFftOutput &output)
{
    for (std::size_t index = 0; index < kFftSize; ++index) {
        output[reverse_seven_bits(index)] = {input[index], 0};
    }

    for (std::size_t span = 2U; span <= kFftSize; span <<= 1U) {
        const std::size_t half = span / 2U;
        const std::size_t twiddle_stride = kFftSize / span;
        for (std::size_t base = 0; base < kFftSize; base += span) {
            for (std::size_t offset = 0; offset < half; ++offset) {
                const ComplexFixed even = output[base + offset];
                const ComplexFixed odd = multiply_twiddle(
                    output[base + offset + half], offset * twiddle_stride);
                output[base + offset] = {
                    round_half(static_cast<std::int64_t>(even.real) + odd.real),
                    round_half(static_cast<std::int64_t>(even.imag) + odd.imag),
                };
                output[base + offset + half] = {
                    round_half(static_cast<std::int64_t>(even.real) - odd.real),
                    round_half(static_cast<std::int64_t>(even.imag) - odd.imag),
                };
            }
        }
    }
}

std::uint32_t fft_power_saturated(const ComplexFixed &value)
{
#ifdef __SYNTHESIS__
#pragma HLS ALLOCATION operation instances = mul limit = 1
#endif
    const std::uint64_t real = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(value.real) * value.real);
    const std::uint64_t imag = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(value.imag) * value.imag);
    return saturate_u32(real + imag);
}

}  // namespace zmpio::step1

