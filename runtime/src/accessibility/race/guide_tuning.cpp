#include "accessibility/race/guide_tuning.h"

#include "accessibility/race/anticipation.h"
#include "runtime_config.h"

namespace a11y::race {
namespace {

constexpr float kPercent = 100.0f;
constexpr float kDegToRad = 3.14159265f / 180.0f;

}  // namespace

float GuideMaxPan() {
    return static_cast<float>(RuntimeConfigFile::AccessibilitySteeringStrength()) / kPercent;
}

float GuideFullLeanRad() {
    return static_cast<float>(RuntimeConfigFile::AccessibilitySteeringLeanAngle()) * kDegToRad;
}

float AnticipationSeconds() {
    const float knob = static_cast<float>(RuntimeConfigFile::AccessibilitySteeringLookAhead()) / kPercent;
    return kMinAnticipationSec + (kAnticipationSec - kMinAnticipationSec) * knob;
}

}  // namespace a11y::race
