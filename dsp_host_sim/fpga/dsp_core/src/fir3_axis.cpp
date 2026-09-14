#include "fir3_axis.h"

#include "fixed_point.h"

namespace zmpio::step1 {

const std::array<std::int16_t, kFirTaps> kFirCoefficientsQ15 = {
    -90, 82, 427, -58, -1742, -995, 5570, 13190,
    13190, 5570, -995, -1742, -58, 427, 82, -90,
};

Fir3Axis::Fir3Axis()
{
    reset();
}

void Fir3Axis::reset()
{
    for (auto &axis : history_) {
        axis.fill(0);
    }
    write_index_ = 0U;
}

AxisSample Fir3Axis::process(const AxisSample &sample)
{
#ifdef __SYNTHESIS__
#pragma HLS ALLOCATION operation instances = mul limit = 1
#endif
    const std::array<std::int16_t, 3> input = {sample.x, sample.y, sample.z};
    std::array<std::int16_t, 3> output{};

    for (std::size_t axis = 0; axis < history_.size(); ++axis) {
        history_[axis][write_index_] = input[axis];
        std::int64_t accumulator = 0;
        for (std::size_t tap = 0; tap < kFirTaps; ++tap) {
            const std::size_t index = (write_index_ + kFirTaps - tap) % kFirTaps;
            accumulator += static_cast<std::int64_t>(history_[axis][index]) *
                           kFirCoefficientsQ15[tap];
        }
        output[axis] = saturate_i16(round_shift_q15(accumulator));
    }

    write_index_ = (write_index_ + 1U) % kFirTaps;
    return {output[0], output[1], output[2]};
}

}  // namespace zmpio::step1

