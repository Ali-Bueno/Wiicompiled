#ifndef MKW_ACCESSIBILITY_RACE_GUIDE_TUNING_H
#define MKW_ACCESSIBILITY_RACE_GUIDE_TUNING_H

namespace a11y::race {

// The steering guide's three player knobs, read from [accessibility] on every call so the F8
// menu and a saved Config.toml both apply at once. Defaults are the play-tested law
// (docs/driving-review.md); re-exposed 2026-09-12 at a player's request.

// Ceiling of the engine pan, 0..1 (steering_strength / 100).
float GuideMaxPan();
// The bearing to the aim point that is the whole lean, radians (steering_lean_angle).
float GuideFullLeanRad();
// Seconds every predictive cue looks ahead: the guide's horizon and the edge cue's margin
// (steering_look_ahead, 0.10 s at 0, the play-tested 0.79 s at 100, linear past it).
float AnticipationSeconds();

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_GUIDE_TUNING_H
