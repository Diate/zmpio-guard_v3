#include "dsp_core.h"

namespace zmpio::step1 {

DspCore::DspCore()
{
    reset();
}

void DspCore::reset()
{
    fir_.reset();
    features_.reset();
    pending_ = {};
    pending_valid_ = false;
    accepted_samples_ = 0U;
    produced_frames_ = 0U;
}

bool DspCore::input_ready() const
{
    return !pending_valid_;
}

bool DspCore::push(const SampleFrameV1 &sample)
{
    if (!input_ready()) {
        return false;
    }

    ++accepted_samples_;
    const AxisSample filtered = fir_.process(acceleration_of(sample));
    if (features_.push(filtered, sample.sample_sequence, pending_)) {
        pending_valid_ = true;
        ++produced_frames_;
    }
    return true;
}

bool DspCore::output_valid() const
{
    return pending_valid_;
}

const FeatureFrameV2 &DspCore::output() const
{
    return pending_;
}

bool DspCore::pop(FeatureFrameV2 &frame)
{
    if (!pending_valid_) {
        return false;
    }
    frame = pending_;
    pending_valid_ = false;
    return true;
}

std::uint64_t DspCore::accepted_samples() const
{
    return accepted_samples_;
}

std::uint64_t DspCore::produced_frames() const
{
    return produced_frames_;
}

}  // namespace zmpio::step1

