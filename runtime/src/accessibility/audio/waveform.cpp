#include "accessibility/audio/waveform.h"

#include <cmath>

namespace a11y::audio {
namespace {

// Equal-RMS trims. For peak A the RMS is A for a square, A/sqrt(2) for a sine and A/sqrt(3) for a
// triangle or saw; normalising to the triangle keeps every trim at or below full scale.
constexpr float kTrimTriangle = 1.0f;
constexpr float kTrimSaw = 1.0f;
constexpr float kTrimSine = 0.816496580927726f;    // sqrt(2/3)
constexpr float kTrimSquare = 0.577350269189626f;  // 1/sqrt(3)

constexpr float kTwoPi = 6.283185307179586f;

}  // namespace

float SampleWaveform(Waveform shape, float phase) {
    switch (shape) {
        case Waveform::Sine:
            return std::sin(phase * kTwoPi) * kTrimSine;
        case Waveform::Square:
            return (phase < 0.5f ? 1.0f : -1.0f) * kTrimSquare;
        case Waveform::Saw:
            return (phase * 2.0f - 1.0f) * kTrimSaw;
        case Waveform::Triangle:
        default:
            return (phase < 0.5f ? phase * 4.0f - 1.0f : 3.0f - phase * 4.0f) * kTrimTriangle;
    }
}

}  // namespace a11y::audio
