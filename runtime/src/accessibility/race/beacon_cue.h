#ifndef MKW_ACCESSIBILITY_RACE_BEACON_CUE_H
#define MKW_ACCESSIBILITY_RACE_BEACON_CUE_H

#include <algorithm>

#include "accessibility/race/anticipation.h"

namespace a11y::race {

struct RaceState;
class CourseMap;

// What every repeating "something is over there" blip shares - the item box, the trick ramp - so
// the two point the same way and pace the same way. They are short cues, so they may pan on their
// own without becoming a second sustained direction language.

// How far ahead a target is still worth mentioning: the spoken lead at the current speed, the same
// reaction budget every other anticipating cue gets, floored at one road width when stopped.
inline constexpr float kBeaconRangeSec = kSpokenLeadSec;
inline constexpr float kBeaconIntervalSec = 0.55f;

inline float BeaconRange(float speedPerSecond, float roadHalfWidth) {
    return std::max(speedPerSecond * kBeaconRangeSec, 2.0f * roadHalfWidth);
}

// The pan is where the target sits ACROSS THE ROAD relative to the kart, in real road half-widths,
// in the same frame the steering guide's centre is defined in: the line's right vector at the
// kart's arc. A kart that drifts right of a ramp on the line hears its engine right and the ramp
// left, by the same amount (player, 2026-09-14). Neither the bearing nor the kart's heading enters:
// a bearing grows as the target nears and swung a ramp dead ahead to one side in the last metres,
// and the heading would move the ramp every time the kart turned. Full pan at one half-width.
float RoadFramePan(const CourseMap& map, float arc, const RaceState& state, float targetX,
                   float targetZ);

// The scale RoadFramePan uses: the measured asphalt when the edge sweep has it, else the corridor.
float BeaconRoadHalfWidth(const CourseMap& map);

// 1 on top of the target, 0 at the edge of the range.
inline float BeaconNearness(float distance, float range) {
    return std::clamp(1.0f - distance / range, 0.0f, 1.0f);
}

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_BEACON_CUE_H
