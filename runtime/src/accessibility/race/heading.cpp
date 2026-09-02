#include "accessibility/race/heading.h"

#include <cmath>

#include "accessibility/race/course_map.h"
#include "accessibility/race/race_state.h"

namespace a11y::race {

void Handedness::Observe(const RaceState& state, const CourseMap& map, int station) {
    (void)state;
    (void)station;
    if (!map.Loaded()) {
        return;
    }
    // The map settled which perpendicular is "right" from the checkpoint's own left-to-right
    // vector; the kart shares the world, so it shares the answer. An earlier draft latched this
    // by watching the kart drive aligned with the track - the same answer, arrived at later and
    // through play rather than data.
    mRightIsFzNegFx = map.RightPerpSign() > 0.0f;
    mKnown = true;
}

void Handedness::RightVector(const RaceState& state, float& x, float& z) const {
    x = 0.0f;
    z = 0.0f;
    if (!mKnown) {
        return;  // no convention yet: a zero vector names no side, the default convention guesses
    }
    x = mRightIsFzNegFx ? state.forwardZ : -state.forwardZ;
    z = mRightIsFzNegFx ? -state.forwardX : state.forwardX;
}

bool Handedness::BearingTo(const RaceState& state, float targetX, float targetZ,
                           float& bearingOut) const {
    if (!mKnown) {
        return false;  // no convention yet, so say nothing rather than guess a side
    }
    const float dx = targetX - state.x;
    const float dz = targetZ - state.z;
    if (dx == 0.0f && dz == 0.0f) {
        return false;
    }

    const float rightX = mRightIsFzNegFx ? state.forwardZ : -state.forwardZ;
    const float rightZ = mRightIsFzNegFx ? -state.forwardX : state.forwardX;
    const float forwardComponent = dx * state.forwardX + dz * state.forwardZ;
    const float rightComponent = dx * rightX + dz * rightZ;
    bearingOut = std::atan2(rightComponent, forwardComponent);
    return true;
}

}  // namespace a11y::race
