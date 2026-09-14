#ifndef ZMPIO_STEP1_FFT128_REAL_H
#define ZMPIO_STEP1_FFT128_REAL_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace zmpio::step1 {

constexpr std::size_t kFftSize = 128U;
constexpr std::size_t kOneSidedBins = (kFftSize / 2U) + 1U;

struct ComplexFixed {
    std::int32_t real;
    std::int32_t imag;
};

using RealFftInput = std::array<std::int32_t, kFftSize>;
using ComplexFftOutput = std::array<ComplexFixed, kFftSize>;

void fft128_real(const RealFftInput &input, ComplexFftOutput &output);
std::uint32_t fft_power_saturated(const ComplexFixed &value);

}  // namespace zmpio::step1

#endif

