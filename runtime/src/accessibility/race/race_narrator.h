#ifndef MKW_ACCESSIBILITY_RACE_RACE_NARRATOR_H
#define MKW_ACCESSIBILITY_RACE_RACE_NARRATOR_H

namespace a11y::race {

struct RaceState;

// Says the things a sighted player reads off the HUD: which lap, what position, and the finish.
//
// This exists because the menu watcher used to narrate the HUD by accident, and now correctly does
// not - so without it the race would say less than before, not more.
//
// Everything is spoken on a transition and never on a value. Position is the awkward one: it
// changes constantly in a pack, and announcing every swap is unusable, so a new position has to
// hold for a moment before it is worth saying.
class RaceNarrator {
public:
    void Reset();
    void Tick(const RaceState& state, float dtSec);

private:
    int mSpokenLap = -1;
    int mSpokenPosition = -1;
    int mPendingPosition = -1;
    float mPendingHeldSec = 0.0f;
    bool mSpokenFinish = false;
};

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_RACE_NARRATOR_H
