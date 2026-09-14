#ifndef ZMPIO_STEP1_SAMPLE_FRAME_V1_H
#define ZMPIO_STEP1_SAMPLE_FRAME_V1_H

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace zmpio::step1 {

#pragma pack(push, 1)
struct SampleFrameV1 {
    std::int16_t accel_x;
    std::int16_t accel_y;
    std::int16_t accel_z;
    std::int16_t temperature;
    std::int16_t gyro_x;
    std::int16_t gyro_y;
    std::int16_t gyro_z;
    std::uint16_t flags;
    std::uint32_t sample_sequence;
};
#pragma pack(pop)

struct AxisSample {
    std::int16_t x;
    std::int16_t y;
    std::int16_t z;
};

static_assert(sizeof(SampleFrameV1) == 20U, "sample frame must be 160 bits");
static_assert(offsetof(SampleFrameV1, flags) == 14U, "flags offset changed");
static_assert(offsetof(SampleFrameV1, sample_sequence) == 16U,
              "sample sequence offset changed");
static_assert(std::is_trivially_copyable<SampleFrameV1>::value,
              "sample frame must remain trivially copyable");

inline AxisSample acceleration_of(const SampleFrameV1 &sample)
{
    return {sample.accel_x, sample.accel_y, sample.accel_z};
}

}  // namespace zmpio::step1

#endif

