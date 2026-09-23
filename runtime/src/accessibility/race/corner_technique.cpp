#include "accessibility/race/corner_technique.h"

#include "accessibility/race/anticipation.h"
#include "accessibility/race/course_map.h"

namespace a11y::race {
namespace {

// Kart::Movement::UpdateRotate (0x8057C69C): grip yaw is stick x tightness x this speed factor,
// which falls from 1 at 20 units/frame to 0.5 at 70 and stays there (docs/class-speed-review.md).
constexpr float kFactorRiseEnd = 20.0f;
constexpr float kFactorFloorStart = 70.0f;
constexpr float kFactorAtRest = 0.4f;
constexpr float kFactorRisePerUnit = 0.03f;
constexpr float kFactorFallPerUnit = 0.01f;
constexpr float kFactorFloor = 0.5f;

// The tightest drift the reference kart can hold: its drift tightness (kartParam.bin, Standard
// Kart M) plus the bonus a fresh drift starts with, half the tightness at full speed ratio and
// decaying from there (docs/class-speed-review.md). A corner tighter than this at top speed cannot
// be made without slowing down.
constexpr float kDriftTightnessPerFrame = 0.01866f;
constexpr float kFreshDriftBonus = 1.5f;

// The radius every grade is measured against (course_curves.cpp kRadiusHard): full stick without
// a drift at the reference speed.
constexpr float kReferenceGripRadius = 6800.0f;

float SpeedFactor(float v) {
    if (v < kFactorRiseEnd) return kFactorAtRest + kFactorRisePerUnit * v;
    if (v < kFactorFloorStart) return 1.0f - kFactorFallPerUnit * (v - kFactorRiseEnd);
    return kFactorFloor;
}

// Grip radius is speed over yaw rate: v / (tightness x factor), so it scales from the reference by
// the ratio of v / factor and the tightness cancels.
float GripRadius(float v) {
    const float reference = kReferenceSpeedUnitsPerFrame / SpeedFactor(kReferenceSpeedUnitsPerFrame);
    return kReferenceGripRadius * (v / SpeedFactor(v)) / reference;
}

float TightestDriftRadius(float v) {
    return v / (kDriftTightnessPerFrame * kFreshDriftBonus);
}

// What the grade word already asks for, on the same scale as the need below.
CornerTechnique GradeTechnique(TurnSeverity severity) {
    switch (severity) {
        case TurnSeverity::Hairpin:
            return CornerTechnique::Brake;
        case TurnSeverity::Hard:
            return CornerTechnique::Drift;
        default:
            return CornerTechnique::None;
    }
}

}  // namespace

CornerTechnique ExtraTechnique(const Curve& curve, float baseSpeed) {
    if (!(baseSpeed > 0.0f) || !(curve.radius > 0.0f)) {
        return CornerTechnique::None;
    }
    const CornerTechnique need = curve.radius >= GripRadius(baseSpeed) ? CornerTechnique::None
                                 : curve.radius >= TightestDriftRadius(baseSpeed)
                                     ? CornerTechnique::Drift
                                     : CornerTechnique::Brake;
    return need > GradeTechnique(curve.severity) ? need : CornerTechnique::None;
}

}  // namespace a11y::race
