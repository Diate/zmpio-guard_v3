#ifndef ZMPIO_STEP1_DSP_CORE_H
#define ZMPIO_STEP1_DSP_CORE_H

#include <cstdint>

#include "feature_extract.h"
#include "fir3_axis.h"
#include "sample_frame_v1.h"

namespace zmpio::step1 {

class DspCore {
public:
    DspCore();

    void reset();
    bool input_ready() const;
    bool push(const SampleFrameV1 &sample);
    bool output_valid() const;
    const FeatureFrameV2 &output() const;
    bool pop(FeatureFrameV2 &frame);

    std::uint64_t accepted_samples() const;
    std::uint64_t produced_frames() const;

private:
    Fir3Axis fir_{};
    FeatureExtractor features_{};
    FeatureFrameV2 pending_{};
    bool pending_valid_ = false;
    std::uint64_t accepted_samples_ = 0U;
    std::uint64_t produced_frames_ = 0U;
};

}  // namespace zmpio::step1

#endif

