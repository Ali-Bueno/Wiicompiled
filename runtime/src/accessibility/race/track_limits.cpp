#include "accessibility/race/track_limits.h"

#include "accessibility/a11y_log.h"
#include "accessibility/audio/cue_service.h"
#include "accessibility/localization.h"
#include "accessibility/race/anticipation.h"
#include "accessibility/race/course_map.h"
#include "accessibility/race/edge_map.h"
#include "accessibility/race/race_state.h"
#include "accessibility/screen_reader.h"
#include "runtime_config.h"

namespace a11y::race {
namespace {

using audio::CueChannel;
using audio::CueService;

// The nearest route segment can jump to a different part of the course - the far side of one of
// Mushroom Gorge's gaps, the other level of a crossover - and the offset's sign jumps with it: a
// real lap logged -4.68 to +6.89 inside one beep interval. A kart cannot cross the line that fast,
// so the panned side only follows a new sign once it has held for longer than such a jump lasts.
constexpr float kEdgeSideHoldSec = kLimitDebounceSec * 2.0f;

// Wrong way is a held state, not an event, so it is re-announced while it holds.
constexpr float kWrongWayRepeatSec = kSpokenLeadSec;

}  // namespace

void TrackLimits::Reset() {
    mBeepTimer = 0.0f;
    mYawRate = 0.0f;
    mLastForwardX = 0.0f;
    mLastForwardZ = 0.0f;
    mHaveLastForward = false;
    mBeepLevel = 0.0f;
    mLastMagnitude = 0.0f;
    mNearEdge = false;
    mHoldingTone = false;
    mHoldingEdge = false;
    mSurfaceOffRoad = false;
    mSurfaceHoldSec = 0.0f;
    mWasOffRoad = false;
    mWasWrongWay = false;
    mWrongWayHoldSec = 0.0f;
    mWrongWaySaySec = 0.0f;
    mSideRight = false;
    mSideKnown = false;
    mSideHoldSec = 0.0f;
    mGradeIsReal = false;
    mNearEdgeKind = EdgeKind::Unknown;
    CueService::Instance().Stop(CueChannel::Edge);
}

// The surface flag has to read the same way for a debounce window before the answer flips, so a
// boundary flicker can neither chop the held tone into a warble nor queue a backlog of speech.
bool TrackLimits::SurfaceSaysOffRoad(bool offRoad, float dtSec) {
    if (offRoad == mSurfaceOffRoad) {
        mSurfaceHoldSec = 0.0f;
        return mSurfaceOffRoad;
    }
    mSurfaceHoldSec += dtSec;
    if (mSurfaceHoldSec < kLimitDebounceSec) {
        return mSurfaceOffRoad;
    }
    mSurfaceHoldSec = 0.0f;
    mSurfaceOffRoad = offRoad;
    return mSurfaceOffRoad;
}

// The ear both edge cues pan to. A side that disagrees with the one the player is hearing has to
// last kEdgeSideHoldSec before it is adopted, which a nearest-segment jump never does.
bool TrackLimits::PannedSideIsRight(bool towardsRight, bool haveOffset, float dtSec) {
    if (!haveOffset) {
        return mSideRight;  // nothing measured this frame: keep the ear the player last heard
    }
    if (!mSideKnown) {
        mSideKnown = true;
        mSideRight = towardsRight;
        mSideHoldSec = 0.0f;
        return mSideRight;
    }
    if (towardsRight == mSideRight) {
        mSideHoldSec = 0.0f;
        return mSideRight;
    }
    mSideHoldSec += dtSec;
    if (mSideHoldSec >= kEdgeSideHoldSec) {
        mSideRight = towardsRight;
        mSideHoldSec = 0.0f;
    }
    return mSideRight;
}

// Driven off the DEBOUNCED surface, not the raw flag: the raw one flickers several times a second
// on a kerb, and every flicker used to queue another sound behind the last.
//
// A sound each way, as Forza does it, instead of the spoken "off road" / "on road" this used to
// be: a word arrives late and sits in the speech queue behind a corner call, and the surface is
// something the player needs the instant it changes. Leaving drops to the beeps' lowest note,
// returning rises to their highest, on the family's own timbre; a drop keeps its saw and its
// octave. Its own channel, so the held tone that follows cannot cut it off.
void TrackLimits::UpdateSurface(bool offRoad) {
    if (offRoad == mWasOffRoad) {
        return;
    }
    mWasOffRoad = offRoad;
    // Temporary, same reason as the edge logs.
    RT_LOGF(RT_TAG_A11Y, "surface: %s (edge kind %s)\n", offRoad ? "off_road" : "on_road",
            EdgeKindName(mNearEdgeKind));
    CueService::Instance().PlayOneShot(CueChannel::Surface, SurfaceChangeCue(offRoad, mSideRight));
}

// Debounced like the surface, and re-announced while it holds: wrong way is a state the player can
// sit in for many seconds, and one utterance at the transition is easily missed under engine noise.
void TrackLimits::UpdateWrongWay(bool wrongWay, float dtSec) {
    if (wrongWay == mWasWrongWay) {
        mWrongWayHoldSec = 0.0f;
    } else {
        mWrongWayHoldSec += dtSec;
        if (mWrongWayHoldSec >= kLimitDebounceSec) {
            mWrongWayHoldSec = 0.0f;
            mWasWrongWay = wrongWay;
            mWrongWaySaySec = 0.0f;  // a genuine change speaks at once
        }
    }
    if (!mWasWrongWay) {
        return;
    }
    mWrongWaySaySec -= dtSec;
    if (mWrongWaySaySec > 0.0f) {
        return;
    }
    mWrongWaySaySec = kWrongWayRepeatSec;
    // Never interrupt: a repeating reminder that cuts off the corner calls would cost the player
    // the very information they need to turn around.
    ScreenReader::Instance().Speak(loc::Get("wrong_way"), /*interrupt=*/false);
}

void TrackLimits::Tick(const RaceState& state, const CourseMap& map, const Handedness& handedness,
                       int station, float dtSec) {
    if (!state.valid || !state.driving) {
        mHaveLastForward = false;
        if (mHoldingTone) {
            CueService::Instance().Stop(CueChannel::Edge);
            mHoldingTone = false;
        }
        return;
    }
    // `state.offRoad` alone is not enough: UpdateOffroad stops writing it while airborne, so it
    // keeps reporting the last surface the kart touched. Debounced once here so the tone and the
    // narration can never disagree about which surface the kart is on.
    const bool offRoad = SurfaceSaysOffRoad(state.offRoad && state.onGround, dtSec);
    // Only the edge cue needs the course map; the surface and the wrong-way flag come from the
    // race record, and gating them on a loaded map silenced both on any course that never built.
    if (map.Loaded() && RuntimeConfigFile::AccessibilityEdgeCues()) {
        UpdateEdge(state, map, handedness, station, dtSec, offRoad);
    } else if (mHoldingTone) {
        CueService::Instance().Stop(CueChannel::Edge);
        mHoldingTone = false;
    }
    UpdateSurface(offRoad);
    UpdateWrongWay(state.wrongWay, dtSec);
}

}  // namespace a11y::race
