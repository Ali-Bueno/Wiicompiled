#include "accessibility/race/track_limits.h"

#include "accessibility/a11y_log.h"
#include "accessibility/audio/cue_service.h"
#include "accessibility/localization.h"
#include "accessibility/race/course_map.h"
#include "accessibility/race/edge_map.h"
#include "accessibility/race/race_state.h"
#include "accessibility/screen_reader.h"
#include "runtime_config.h"

namespace a11y::race {

using audio::CueChannel;
using audio::CueService;

void TrackLimits::Reset() {
    mBeepTimer = 0.0f;
    mBeepLevel = 0.0f;
    mNearEdge = false;
    mHoldingTone = false;
    mWasOffRoad = false;
    mWasWrongWay = false;
    mSurfaceHoldSec = 0.0f;
    mSideRight = false;
    mSideKnown = false;
    mSideHoldSec = 0.0f;
    mGradeIsReal = false;
    mNearEdgeKind = EdgeKind::Unknown;
    CueService::Instance().Stop(CueChannel::Edge);
}

void TrackLimits::UpdateSurface(const RaceState& state) {
    if (state.offRoad == mWasOffRoad) {
        return;
    }
    mWasOffRoad = state.offRoad;
    // Temporary, same reason as the edge logs.
    RT_LOGF(RT_TAG_A11Y, "surface: %s (edge kind %s)\n", state.offRoad ? "off_road" : "on_road",
            EdgeKindName(mNearEdgeKind));
    // Spoken on the transition only. Speaking a continuous value is what turns narration into
    // spam, and the surface either is or is not slowing the kart. Leaving the road where the
    // measured edge on that side is a drop is a different sentence from sliding onto grass - the
    // kind comes from the edge cue's own last look, so it is only known while edge cues are on.
    const char* key = "on_road";
    if (state.offRoad) {
        key = mNearEdgeKind == EdgeKind::Fall ? "off_road_fall" : "off_road";
    }
    ScreenReader::Instance().Speak(loc::Get(key), /*interrupt=*/false);
}

void TrackLimits::UpdateWrongWay(const RaceState& state) {
    if (state.wrongWay == mWasWrongWay) {
        return;
    }
    mWasWrongWay = state.wrongWay;
    if (mWasWrongWay) {
        ScreenReader::Instance().Speak(loc::Get("wrong_way"), /*interrupt=*/true);
    }
}

void TrackLimits::Tick(const RaceState& state, const CourseMap& map, const Handedness& handedness,
                       int station, float dtSec) {
    if (!state.valid || !state.driving || !map.Loaded()) {
        if (mHoldingTone) {
            CueService::Instance().Stop(CueChannel::Edge);
            mHoldingTone = false;
        }
        return;
    }
    if (RuntimeConfigFile::AccessibilityEdgeCues()) {
        UpdateEdge(state, map, handedness, station, dtSec);
    } else if (mHoldingTone) {
        CueService::Instance().Stop(CueChannel::Edge);
        mHoldingTone = false;
    }
    UpdateSurface(state);
    UpdateWrongWay(state);
}

}  // namespace a11y::race
