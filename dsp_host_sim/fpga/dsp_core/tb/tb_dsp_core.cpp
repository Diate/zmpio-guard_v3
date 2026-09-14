#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "dsp_core.h"
#include "feature_frame_v2.h"
#include "fft128_real.h"
#include "fir3_axis.h"
#include "fixed_point.h"
#include "sample_frame_v1.h"

namespace {

using namespace zmpio::step1;

int failures = 0;

void check(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void test_contract_sizes()
{
    check(sizeof(SampleFrameV1) == 20U, "sample_frame_v1 is not 160 bits");
    check(sizeof(FeatureFrameV2) == 48U, "feature_frame_v2 is not 48 bytes");
}

void test_fir_impulse()
{
    constexpr std::array<std::int16_t, kFirTaps> expected = {
        -45, 41, 214, -29, -871, -498, 2785, 6595,
        6595, 2785, -498, -871, -29, 214, 41, -45,
    };
    Fir3Axis fir;
    for (std::size_t index = 0; index < kFirTaps; ++index) {
        const AxisSample input = {
            static_cast<std::int16_t>(index == 0U ? 16384 : 0),
            static_cast<std::int16_t>(index == 0U ? -16384 : 0),
            0,
        };
        const AxisSample output = fir.process(input);
        check(output.x == expected[index], "FIR X impulse mismatch at tap " +
                                                   std::to_string(index));
        check(output.y == static_cast<std::int16_t>(-expected[index]),
              "FIR Y impulse mismatch at tap " + std::to_string(index));
        check(output.z == 0, "FIR Z impulse must remain zero");
    }
}

std::int32_t reference_round_q15(std::int64_t value)
{
    return value >= 0
               ? static_cast<std::int32_t>((value + 16384LL) / 32768LL)
               : -static_cast<std::int32_t>(((-value) + 16384LL) / 32768LL);
}

std::int16_t reference_saturate_i16(std::int64_t value)
{
    value = std::max<std::int64_t>(value, std::numeric_limits<std::int16_t>::min());
    value = std::min<std::int64_t>(value, std::numeric_limits<std::int16_t>::max());
    return static_cast<std::int16_t>(value);
}

void test_fir_step_and_random()
{
    Fir3Axis step_fir;
    AxisSample last{};
    for (std::size_t index = 0; index < 64U; ++index) {
        last = step_fir.process({12000, -8000, 4000});
    }
    check(std::abs(static_cast<int>(last.x) - 12000) <= 1,
          "FIR settled X step is not unity gain");
    check(std::abs(static_cast<int>(last.y) + 8000) <= 1,
          "FIR settled Y step is not unity gain");
    check(std::abs(static_cast<int>(last.z) - 4000) <= 1,
          "FIR settled Z step is not unity gain");

    Fir3Axis random_fir;
    std::array<std::array<std::int16_t, kFirTaps>, 3> history{};
    std::size_t write_index = 0U;
    std::mt19937 random(0x46524952U);
    std::uniform_int_distribution<int> values(-32768, 32767);
    for (std::size_t vector = 0; vector < 4096U; ++vector) {
        const AxisSample input = {
            static_cast<std::int16_t>(values(random)),
            static_cast<std::int16_t>(values(random)),
            static_cast<std::int16_t>(values(random)),
        };
        const AxisSample actual = random_fir.process(input);
        const std::array<std::int16_t, 3> lanes = {input.x, input.y, input.z};
        std::array<std::int16_t, 3> expected{};
        for (std::size_t axis = 0; axis < 3U; ++axis) {
            history[axis][write_index] = lanes[axis];
            std::int64_t accumulator = 0;
            for (std::size_t tap = 0; tap < kFirTaps; ++tap) {
                const std::size_t index =
                    (write_index + kFirTaps - tap) % kFirTaps;
                accumulator += static_cast<std::int64_t>(history[axis][index]) *
                               kFirCoefficientsQ15[tap];
            }
            expected[axis] = reference_saturate_i16(
                reference_round_q15(accumulator));
        }
        write_index = (write_index + 1U) % kFirTaps;
        check(actual.x == expected[0] && actual.y == expected[1] &&
                  actual.z == expected[2],
              "FIR random bit-accurate mismatch at vector " +
                  std::to_string(vector));
    }
}

double fir_gain_db(double frequency_hz)
{
    constexpr double pi = 3.14159265358979323846;
    double real = 0.0;
    double imag = 0.0;
    for (std::size_t tap = 0; tap < kFirTaps; ++tap) {
        const double angle = -2.0 * pi * frequency_hz *
                             static_cast<double>(tap) / 100.0;
        const double coefficient =
            static_cast<double>(kFirCoefficientsQ15[tap]) / 32768.0;
        real += coefficient * std::cos(angle);
        imag += coefficient * std::sin(angle);
    }
    const double magnitude = std::sqrt(real * real + imag * imag);
    return 20.0 * std::log10(magnitude);
}

void test_fir_response()
{
    check(std::abs(fir_gain_db(0.0)) < 0.01, "FIR DC gain is not unity");
    for (double frequency = 0.0; frequency <= 15.0; frequency += 0.25) {
        check(fir_gain_db(frequency) > -1.0,
              "FIR exceeds 1 dB loss in the 0-15 Hz sweep");
    }
    for (double frequency = 35.0; frequency <= 50.0; frequency += 0.25) {
        check(fir_gain_db(frequency) < -30.0,
              "FIR attenuation is below 30 dB in the 35-50 Hz sweep");
    }
}

std::size_t dominant_fft_bin(const RealFftInput &input,
                             std::size_t first_bin,
                             std::size_t last_bin)
{
    ComplexFftOutput output{};
    fft128_real(input, output);
    std::size_t dominant = first_bin;
    std::uint32_t maximum = 0U;
    for (std::size_t bin = first_bin; bin <= last_bin; ++bin) {
        const std::uint32_t power = fft_power_saturated(output[bin]);
        if (power > maximum) {
            maximum = power;
            dominant = bin;
        }
    }
    return dominant;
}

void test_fft_center_bin()
{
    constexpr double pi = 3.14159265358979323846;
    RealFftInput input{};
    for (std::size_t index = 0; index < kFftSize; ++index) {
        input[index] = static_cast<std::int32_t>(std::lround(
            10000.0 * std::cos(2.0 * pi * 10.0 * static_cast<double>(index) /
                               static_cast<double>(kFftSize))));
    }

    ComplexFftOutput output{};
    fft128_real(input, output);
    std::size_t dominant = 1U;
    std::uint32_t maximum = 0U;
    for (std::size_t bin = 1U; bin <= kFftSize / 2U; ++bin) {
        const std::uint32_t power = fft_power_saturated(output[bin]);
        if (power > maximum) {
            maximum = power;
            dominant = bin;
        }
    }
    check(dominant == 10U, "FFT center tone did not land in bin 10");
    check(maximum > 24000000U, "FFT center-tone power is unexpectedly small");
    check(std::abs(output[10].imag) < 8, "FFT center-tone imaginary leakage is large");
}

void test_fft_dc_nyquist_off_bin_and_multitone()
{
    constexpr double pi = 3.14159265358979323846;

    RealFftInput dc{};
    dc.fill(12345);
    check(dominant_fft_bin(dc, 0U, 64U) == 0U,
          "FFT DC vector did not land in bin 0");

    RealFftInput nyquist{};
    for (std::size_t index = 0; index < kFftSize; ++index) {
        nyquist[index] = index % 2U == 0U ? 8000 : -8000;
    }
    check(dominant_fft_bin(nyquist, 0U, 64U) == 64U,
          "FFT Nyquist vector did not land in bin 64");

    RealFftInput off_bin{};
    for (std::size_t index = 0; index < kFftSize; ++index) {
        off_bin[index] = static_cast<std::int32_t>(std::lround(
            9000.0 * std::cos(2.0 * pi * 10.5 * static_cast<double>(index) /
                              static_cast<double>(kFftSize))));
    }
    const std::size_t off_bin_dominant = dominant_fft_bin(off_bin, 1U, 64U);
    check(off_bin_dominant == 10U || off_bin_dominant == 11U,
          "FFT off-bin tone did not split around bins 10/11");

    RealFftInput multitone{};
    for (std::size_t index = 0; index < kFftSize; ++index) {
        multitone[index] = static_cast<std::int32_t>(std::lround(
            9000.0 * std::cos(2.0 * pi * 5.0 * static_cast<double>(index) /
                             static_cast<double>(kFftSize)) +
            3000.0 * std::cos(2.0 * pi * 18.0 * static_cast<double>(index) /
                             static_cast<double>(kFftSize))));
    }
    ComplexFftOutput spectrum{};
    fft128_real(multitone, spectrum);
    check(dominant_fft_bin(multitone, 1U, 64U) == 5U,
          "FFT multitone dominant component is not bin 5");
    check(fft_power_saturated(spectrum[18]) > 1500000U,
          "FFT multitone secondary component in bin 18 is missing");
}

SampleFrameV1 tone_sample(std::uint32_t sequence)
{
    constexpr double pi = 3.14159265358979323846;
    const double phase = 2.0 * pi * 10.0 * static_cast<double>(sequence) / 100.0;
    SampleFrameV1 sample{};
    sample.accel_x = 100;
    sample.accel_y = -50;
    sample.accel_z = static_cast<std::int16_t>(
        std::lround(4096.0 + 1000.0 * std::sin(phase)));
    sample.sample_sequence = sequence;
    return sample;
}

void test_feature_tone()
{
    DspCore core;
    std::vector<FeatureFrameV2> frames;
    for (std::uint32_t sequence = 0U; sequence < 384U; ++sequence) {
        check(core.input_ready(), "core unexpectedly stalled without back-pressure");
        check(core.push(tone_sample(sequence)), "core rejected a ready input");
        if (core.output_valid()) {
            FeatureFrameV2 frame{};
            check(core.pop(frame), "feature output could not be popped");
            frames.push_back(frame);
        }
    }

    check(frames.size() == 5U, "384 samples must produce five overlapped frames");
    if (!frames.empty()) {
        const FeatureFrameV2 &frame = frames.back();
        const double dominant_hz =
            static_cast<double>(frame.dominant_frequency_q16_16) / 65536.0;
        check(dominant_hz > 9.0 && dominant_hz < 11.0,
              "feature dominant frequency is not the injected 10 Hz tone");
        check(frame.rms_q24_8 > 0U, "feature RMS must be nonzero");
        check(frame.variance_q32_0 > 0U, "feature variance must be nonzero");
        const auto largest_band = std::max_element(
            std::begin(frame.band_energy_q32_0),
            std::end(frame.band_energy_q32_0));
        check(std::distance(std::begin(frame.band_energy_q32_0), largest_band) == 2,
              "10 Hz energy must dominate band 13-18 after FFT binning");
    }
}

void test_flat_signal_has_no_spectral_feature()
{
    DspCore core;
    FeatureFrameV2 last{};
    bool have_frame = false;
    for (std::uint32_t sequence = 0U; sequence < 512U; ++sequence) {
        SampleFrameV1 sample{};
        sample.accel_z = 4096;
        sample.sample_sequence = sequence;
        check(core.push(sample), "flat input was rejected");
        if (core.output_valid()) {
            check(core.pop(last), "flat-input feature could not be consumed");
            have_frame = true;
        }
    }
    check(have_frame, "flat input did not produce a feature frame");
    check(last.rms_q24_8 == 0U, "settled flat input has nonzero RMS");
    check(last.peak_q24_8 == 0U, "settled flat input has nonzero peak");
    check(last.variance_q32_0 == 0U, "settled flat input has nonzero variance");
    check(last.kurtosis_q16_16 == 0U, "settled flat input has nonzero kurtosis");
    check(last.dominant_power_q32_0 == 0U,
          "settled flat input has nonzero dominant power");
    check(last.dominant_frequency_q16_16 == 0U,
          "zero-power spectrum must report zero dominant frequency");
    check(std::all_of(std::begin(last.band_energy_q32_0),
                      std::end(last.band_energy_q32_0),
                      [](std::uint32_t value) { return value == 0U; }),
          "settled flat input has nonzero band energy");
}

void test_random_backpressure()
{
    DspCore core;
    std::mt19937 random(0x5A4D5049U);
    std::bernoulli_distribution consumer_ready(0.35);
    std::uint32_t next_sequence = 0U;
    std::vector<std::uint32_t> window_ends;
    std::size_t cycles = 0U;

    while ((next_sequence < 512U || core.output_valid()) && cycles < 20000U) {
        if (core.output_valid() && consumer_ready(random)) {
            FeatureFrameV2 frame{};
            check(core.pop(frame), "valid output was not consumable");
            window_ends.push_back(frame.window_end_sample_sequence);
        }
        if (next_sequence < 512U && core.input_ready()) {
            check(core.push(tone_sample(next_sequence)),
                  "ready core rejected input under back-pressure");
            ++next_sequence;
        }
        ++cycles;
    }

    const std::vector<std::uint32_t> expected = {127U, 191U, 255U, 319U,
                                                 383U, 447U, 511U};
    check(cycles < 20000U, "random back-pressure deadlocked the core");
    check(core.accepted_samples() == 512U, "back-pressure lost an input sample");
    check(core.produced_frames() == expected.size(),
          "back-pressure changed the feature frame count");
    check(window_ends == expected, "back-pressure changed output sequence order");
}

void test_saturation_path()
{
    DspCore core;
    for (std::uint32_t sequence = 0U; sequence < 192U; ++sequence) {
        SampleFrameV1 sample{};
        sample.accel_x = sequence % 2U == 0U ? 32767 : -32768;
        sample.accel_y = 32767;
        sample.accel_z = -32768;
        sample.sample_sequence = sequence;
        check(core.push(sample), "saturation vector was rejected");
        if (core.output_valid()) {
            FeatureFrameV2 frame{};
            check(core.pop(frame), "saturation feature could not be consumed");
        }
    }
    check(core.accepted_samples() == 192U, "saturation test lost samples");
}

}  // namespace

int main()
{
    test_contract_sizes();
    test_fir_impulse();
    test_fir_step_and_random();
    test_fir_response();
    test_fft_center_bin();
    test_fft_dc_nyquist_off_bin_and_multitone();
    test_feature_tone();
    test_flat_signal_has_no_spectral_feature();
    test_random_backpressure();
    test_saturation_path();

    if (failures != 0) {
        std::cerr << failures << " Step1 regression assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "Step1 DSP regression PASS\n";
    return EXIT_SUCCESS;
}
