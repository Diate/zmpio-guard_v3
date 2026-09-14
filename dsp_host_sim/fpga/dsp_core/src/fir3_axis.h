#ifndef ZMPIO_STEP1_FIR3_AXIS_H
#define ZMPIO_STEP1_FIR3_AXIS_H

#include <array>
#include <cstddef>
#include <cstdint>

#include "sample_frame_v1.h"

namespace zmpio::step1 {

constexpr std::size_t kFirTaps = 16U;

extern const std::array<std::int16_t, kFirTaps> kFirCoefficientsQ15;

class Fir3Axis {
public:
    Fir3Axis();

    void reset();
    AxisSample process(const AxisSample &sample);

private:
    std::array<std::array<std::int16_t, kFirTaps>, 3> history_{};
    std::size_t write_index_ = 0U;
};

}  // namespace zmpio::step1

#endif

