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

inline float AnticipationSeconds() {
    const float knob =
        std::clamp(static_cast<float>(RuntimeConfigFile::AccessibilitySteeringLookAhead()), 0.0f,
                   100.0f) /
        100.0f;
    return kAnticipationNearSec + (kAnticipationFarSec - kAnticipationNearSec) * knob;
}

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_ANTICIPATION_H
