#ifndef MKW_ACCESSIBILITY_RACE_KART_TURNING_H
#define MKW_ACCESSIBILITY_RACE_KART_TURNING_H

#include "accessibility/race/anticipation.h"

namespace a11y::race {

// Heading the kart turns per frame at full stick without a drift, at top speed (70+ units/frame).
// Measured in play 2026-09-23 over 18-frame holds, the player's kart and the test bot's alike:
// 2.1x the stick term of Kinoko's KartMove::calcRotation (the reference kart's manual handling
// 0.02335 x 0.5 rad), the body's own spin supplying the rest (docs/class-speed-review.md).
inline constexpr float kGripYawDegPerFrame = 1.40f;
inline constexpr float kGripYawRadPerFrame = kGripYawDegPerFrame * 3.14159265f / 180.0f;
inline constexpr float kModelGripYawRadPerFrame = 0.02335f * 0.5f;
// Measured over modelled, applied to the drift's modelled yaw too.
inline constexpr float kMeasuredYawGain = kGripYawRadPerFrame / kModelGripYawRadPerFrame;

// The grip turn's speed factor (Kinoko KartMove::calcRotation): 0.4 at rest, 1 at 20 units/frame,
// falling to 0.5 at 70 and flat above, so a slow kart turns up to twice as fast as one at speed.
inline constexpr float kGripFactorAtRest = 0.4f;
inline constexpr float kGripFactorRisePerUnit = 0.03f;
inline constexpr float kGripFactorRiseEnd = 20.0f;
inline constexpr float kGripFactorFallPerUnit = 0.01f;
inline constexpr float kGripFactorFloorStart = 70.0f;
inline constexpr float kGripFactorAtSpeed = 0.5f;
inline float GripTurnFactor(float speedPerFrame) {
    const float v = speedPerFrame < 0.0f ? -speedPerFrame : speedPerFrame;
    if (v < kGripFactorRiseEnd) return kGripFactorAtRest + kGripFactorRisePerUnit * v;
    if (v < kGripFactorFloorStart) return 1.0f - kGripFactorFallPerUnit * (v - kGripFactorRiseEnd);
    return kGripFactorAtSpeed;
}

// The radius full stick holds at the reference speed, about 3,260 units: every corner grade is
// measured against it.
inline constexpr float kReferenceGripRadius = kReferenceSpeedUnitsPerFrame / kGripYawRadPerFrame;

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_KART_TURNING_H
