#include "accessibility/race/corner_technique.h"

#include "accessibility/race/anticipation.h"
#include "accessibility/race/course_map.h"
#include "accessibility/race/kart_turning.h"

namespace a11y::race {
namespace {

// The tightest drift the reference kart can hold: its drift tightness (kartParam.bin, Standard
// Kart M) plus the bonus a fresh drift starts with, half the tightness at full speed ratio and
// decaying from there, turned by the same measured gain as grip (docs/class-speed-review.md). A
// corner tighter than this at top speed cannot be made without slowing down.
constexpr float kDriftTightnessPerFrame = 0.01866f;
constexpr float kFreshDriftBonus = 1.5f;

// Grip radius is speed over yaw rate: v / (tightness x factor), so it scales from the reference by
// the ratio of v / factor and the tightness cancels.
float GripRadius(float v) {
    const float reference = kReferenceSpeedUnitsPerFrame / GripTurnFactor(kReferenceSpeedUnitsPerFrame);
    return kReferenceGripRadius * (v / GripTurnFactor(v)) / reference;
}

float TightestDriftRadius(float v) {
    return v / (kDriftTightnessPerFrame * kFreshDriftBonus * kMeasuredYawGain);
}

// What the grade word already asks for, on the same scale as the need below.
CornerTechnique GradeTechnique(TurnSeverity severity) {
    switch (severity) {
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
