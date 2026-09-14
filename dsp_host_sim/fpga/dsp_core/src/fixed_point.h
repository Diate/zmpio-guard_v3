#ifndef ZMPIO_STEP1_FIXED_POINT_H
#define ZMPIO_STEP1_FIXED_POINT_H

#include <cstdint>
#include <limits>

namespace zmpio::step1 {

inline std::int32_t round_shift_q15(std::int64_t value)
{
    if (value >= 0) {
        return static_cast<std::int32_t>((value + (1LL << 14)) >> 15);
    }
    return -static_cast<std::int32_t>(((-value) + (1LL << 14)) >> 15);
}

inline std::int32_t round_half(std::int64_t value)
{
    if (value >= 0) {
        return static_cast<std::int32_t>((value + 1LL) >> 1);
    }
    return -static_cast<std::int32_t>(((-value) + 1LL) >> 1);
}

inline std::int16_t saturate_i16(std::int64_t value)
{
    if (value > std::numeric_limits<std::int16_t>::max()) {
        return std::numeric_limits<std::int16_t>::max();
    }
    if (value < std::numeric_limits<std::int16_t>::min()) {
        return std::numeric_limits<std::int16_t>::min();
    }
    return static_cast<std::int16_t>(value);
}

inline std::uint32_t saturate_u32(std::uint64_t value)
{
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return static_cast<std::uint32_t>(value);
}

inline std::uint32_t integer_sqrt_u64(std::uint64_t value)
{
    std::uint64_t result = 0;
    std::uint64_t bit = 1ULL << 62;

    while (bit > value) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return static_cast<std::uint32_t>(result);
}

}  // namespace zmpio::step1

#endif

