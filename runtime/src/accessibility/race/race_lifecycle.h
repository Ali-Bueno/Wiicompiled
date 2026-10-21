#ifndef MKW_ACCESSIBILITY_RACE_RACE_LIFECYCLE_H
#define MKW_ACCESSIBILITY_RACE_RACE_LIFECYCLE_H

#include "accessibility/race/race_state.h"

namespace a11y::race {

// Unreadable frames and pauses preserve the race; a new kart or start re-arms its services.
class RaceLifecycle {
public:
    void Reset() { *this = RaceLifecycle{}; }

    bool StartsRace(const RaceState& state) {
        if (!state.valid) {
            return false;
        }
        const bool starts = !mHaveRace || state.kartObject != mKart ||
                            (mActive && state.countdownFrames > 0) ||
                            (mFinished && state.driving);
        if (starts) {
            mActive = false;
            mFinished = false;
        }
        mHaveRace = true;
        mKart = state.kartObject;
        mActive = mActive || state.driving || state.finished;
        mFinished = mFinished || state.finished;
        return starts;
    }

private:
    bool mHaveRace = false;
    bool mActive = false;
    bool mFinished = false;
    std::uint32_t mKart = 0;
};

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_RACE_LIFECYCLE_H
