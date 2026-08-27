#ifndef MKW_ACCESSIBILITY_RACE_RACE_RECORD_H
#define MKW_ACCESSIBILITY_RACE_RACE_RECORD_H

namespace a11y::race {

struct RaceState;

// Fills in what the game itself already tracks about the race: the stage, the countdown, and the
// local player's lap, position, checkpoint and progress.
//
// Optional by design. The kart alone is enough to drive the audio cues, so if this cannot be read
// the assists still work and only what can be *said* is narrower.
void FillRaceRecord(RaceState& state);

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_RACE_RECORD_H
