#ifndef MKW_ACCESSIBILITY_RACE_ANTICIPATION_H
#define MKW_ACCESSIBILITY_RACE_ANTICIPATION_H

#include <algorithm>

#include "runtime_config.h"

namespace a11y::race {

// One anticipation budget for every cue that predicts where the kart will be: the steering guide's
// horizon and the edge cue's margin both look this many seconds ahead. Seconds, never distance, so
// the lead is the same at 50cc and at 500cc - what the player spends is hearing, deciding and
// moving the stick, and that is a time.
//
// The range is the play-tested steering horizon, converted at the racing speed it was tuned at
// (a 1300-unit half-width at 6590 units/s): 0.5 widths = 0.10 s, 4 widths = 0.79 s. The player
// settled at the top of the knob, so 100 is the default.
inline constexpr float kAnticipationNearSec = 0.10f;
inline constexpr float kAnticipationFarSec = 0.79f;

// The longest lead any spoken instruction is planned at: the corner call has to land, be understood
// and still leave time to act - a play-test found 2.5 s arriving half a second late. Wrong-way
// reminders repeat on the same gap.
inline constexpr float kSpokenLeadSec = 4.0f;

// The corner countdown: one beep at each of these leads, seconds at the current speed.
inline constexpr float kCountdownLeadSec[] = {2.5f, 1.4f, 0.7f};
inline constexpr int kCountdownStages = 3;

// Where a rule has to be a property of the COURSE - which corners form a run, which gap is a
// straight - it is stated as a distance, the same at every engine class, by the player's
// decision (2026-09-03). The conversion uses one reference speed: the game's Standard Kart M
// with Mario at 150cc, 79.77 units/frame at 50 fps (kartParam.bin + driverParam.bin, the
// vehicle the radius ladder in course_curves.cpp is derived from too).
inline constexpr float kReferenceSpeedUnitsPerSec = 79.77f * 50.0f;
// A straight is a gap the whole countdown fits in at the reference speed; a shorter gap chains
// the next corner to the previous one (it becomes a follower: no call of its own, no countdown).
inline constexpr float kStraightUnits = kReferenceSpeedUnitsPerSec * kCountdownLeadSec[0];

inline float AnticipationSeconds() {
    const float knob =
        std::clamp(static_cast<float>(RuntimeConfigFile::AccessibilitySteeringLookAhead()), 0.0f,
                   100.0f) /
        100.0f;
    return kAnticipationNearSec + (kAnticipationFarSec - kAnticipationNearSec) * knob;
}

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_ANTICIPATION_H
