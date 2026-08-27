#include "accessibility/race/race_narrator.h"

#include <string>

#include "accessibility/race/phrases.h"
#include "accessibility/race/race_state.h"
#include "accessibility/screen_reader.h"

namespace a11y::race {
namespace {

// How long a new position has to hold before it is spoken. Long enough that trading places twice
// through a corner stays silent, short enough that a real overtake is still news.
constexpr float kPositionHoldSec = 0.8f;

// Twelve karts race in Mario Kart Wii, so nothing above this is a position.
constexpr int kMaxPosition = 12;

std::string Ordinal(int value) {
    const std::string digits = std::to_string(value);
    if (SpeakSpanish()) {
        return "puesto " + digits;
    }
    // 11th, 12th and 13th break the pattern the last digit would otherwise give.
    const int lastTwo = value % 100;
    if (lastTwo >= 11 && lastTwo <= 13) {
        return digits + "th";
    }
    switch (value % 10) {
        case 1:
            return digits + "st";
        case 2:
            return digits + "nd";
        case 3:
            return digits + "rd";
        default:
            return digits + "th";
    }
}

void Say(const std::string& text) {
    // Queued, not interrupting. These arrive seconds apart and each one still matters when it is
    // heard a moment late - unlike a menu value, which is stale as soon as the cursor moves on.
    ScreenReader::Instance().Speak(text, /*interrupt=*/false);
}

}  // namespace

void RaceNarrator::Reset() {
    mSpokenLap = -1;
    mSpokenPosition = -1;
    mPendingPosition = -1;
    mPendingHeldSec = 0.0f;
    mSpokenFinish = false;
}

void RaceNarrator::Tick(const RaceState& state, float dtSec) {
    if (!state.valid) {
        return;
    }

    if (state.finished) {
        if (!mSpokenFinish) {
            mSpokenFinish = true;
            const bool es = SpeakSpanish();
            if (state.position >= 1 && state.position <= kMaxPosition) {
                Say(es ? ("meta, " + Ordinal(state.position)) : ("finished " + Ordinal(state.position)));
            } else {
                Say(es ? "meta" : "finished");
            }
        }
        return;
    }

    if (!state.driving) {
        return;
    }

    if (state.lap >= 1 && state.lap != mSpokenLap) {
        mSpokenLap = state.lap;
        // The first lap is not announced: the countdown and the start are unmistakable already, and
        // saying it competes with the go signal.
        if (state.lap > 1 && state.totalLaps > 0) {
            const std::string lap = std::to_string(state.lap);
            const std::string total = std::to_string(state.totalLaps);
            Say(SpeakSpanish() ? ("vuelta " + lap + " de " + total)
                               : ("lap " + lap + " of " + total));
        }
    }

    if (state.position < 1 || state.position > kMaxPosition) {
        return;
    }
    if (state.position == mSpokenPosition) {
        mPendingPosition = -1;
        mPendingHeldSec = 0.0f;
        return;
    }

    if (state.position != mPendingPosition) {
        mPendingPosition = state.position;
        mPendingHeldSec = 0.0f;
        return;
    }

    mPendingHeldSec += dtSec;
    if (mPendingHeldSec < kPositionHoldSec) {
        return;
    }

    mSpokenPosition = mPendingPosition;
    mPendingPosition = -1;
    mPendingHeldSec = 0.0f;
    // The first position of a race is spoken too: a blind player has no grid to look at.
    Say(Ordinal(mSpokenPosition));
}

}  // namespace a11y::race
