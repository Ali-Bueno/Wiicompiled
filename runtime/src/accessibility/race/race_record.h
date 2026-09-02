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

// Drops the frame counter the pause test diffs against, so the first frame of a new race is not
// compared with the last frame of the previous one.
void ResetRaceRecord();

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_RACE_RECORD_H
