#include "accessibility/race/drive_assist.h"
#include <algorithm>
#include <cmath>
#include "accessibility/race/anticipation.h"
#include "accessibility/race/course_map.h"
#include "accessibility/race/edge_map.h"
#include "accessibility/race/heading.h"
#include "accessibility/race/race_state.h"

namespace a11y::race {
namespace {
// ~100 ms is the 60 fps equivalent of the play-tested 0.15-per-frame filter, kept as a time.
constexpr float kPanSmoothTauSec = 0.1f;
// The bearing that is the whole lean: 30 degrees, Forza's own at-speed threshold for "way off the
// line" (`assistfocuscarangletolookahead_minpointspeedramped`). Scaled by angle, not by road, so a
// normal corner's aim point sits near the full lean, a gentle bend halfway, a drift a few degrees.
constexpr float kFullLeanRad = 30.0f * (3.14159265f / 180.0f);
// Astern hysteresis on the +-180 degree seam, from the edge-recovery work (2026-09-09).
constexpr float kAsternEnterRad = 150.0f * (3.14159265f / 180.0f);
constexpr float kAsternExitRad = 120.0f * (3.14159265f / 180.0f);
// Ceiling on the aim distance: past a quarter lap on a closed loop the aim point starts coming
// back round towards the kart, so a bullet on a short course could place it behind.
constexpr float kMaxHorizonLapFraction = 0.25f;
}  // namespace

void DriveAssist::Reset() {
    mPan = 0.0f;
    mLastBearingDeg = 0.0f;
    mLastHorizonUnits = 0.0f;
    mAstern = false;
    mAsternPanSign = 0.0f;
    mCurveGeneration = 0;
    mCues.clear();
    mLastArc = -1.0f;
    mPendingLandmarks.clear();
    mPendingRight.clear();
    mLandmarkBusySec = 0.0f;
}

// Pure pursuit, as Forza's guide and as the first completed blind race here (2026-08-27): the
// bearing to an aim point on the safe line one anticipation time ahead. A corner is heard as the
// aim point swings into it, earlier and harder the tighter it is; a kart returning to the line on
// a good heading points at the aim point and hears near silence, which is the return, not a fault.
// Position and velocity terms were both tried and both zigzagged (2026-09-09): any term that flips
// as soon as the player's correction takes effect makes the player chase the sound.
//
// Sign, specified by the player (2026-08-27): the engine marks the side the kart is heading for -
// "curva a la izquierda -> motor a la derecha" - so steering away from the sound is the fix. An
// aim point on the left means the kart is heading for the right of it.
void DriveAssist::UpdateSteering(const RaceState& state, const CourseMap& map,
                                 const Handedness& handedness, int station, float dtSec) {
    if (dtSec <= 0.0f) return;
    const float alpha = 1.0f - std::exp(-dtSec / kPanSmoothTauSec);
    const float arc = map.ArcOfPosition(state.x, state.z, station);
    float rx = 0.0f, rz = 0.0f;
    handedness.RightVector(state, rx, rz);

    float pan = 0.0f;
    // RouteBased, not Loaded: the checkpoint-midpoint fallback carries progress and corner shape
    // but its midpoints can sit off the road entirely.
    bool have = map.RouteBased() && handedness.Known();
    if (have) {
        // Seconds ahead at the current speed, floored at one real road half-width so a kart
        // stopped or spun still aims at something in front of it.
        float floorUnits = EdgeMap::MedianHalfWidth();
        if (floorUnits <= 0.0f) floorUnits = map.MedianHalfWidth();
        float horizon = std::max(std::fabs(state.speedPerSecond) * AnticipationSeconds(), floorUnits);
        if (map.LapLength() > 0.0f)
            horizon = std::min(horizon, map.LapLength() * kMaxHorizonLapFraction);
        float aimX = 0.0f, aimZ = 0.0f;
        have = map.PointAtArc(map.WrapForward(arc + horizon), aimX, aimZ);
        if (have) {
            const float dx = aimX - state.x, dz = aimZ - state.z;
            // Positive with the aim point on the kart's right.
            const float bearing = std::atan2(dx * rx + dz * rz,
                                             dx * state.forwardX + dz * state.forwardZ);
            float sign = bearing > 0.0f ? -1.0f : 1.0f;
            if (std::fabs(bearing) >= kAsternEnterRad) {
                if (!mAstern) { mAsternPanSign = sign; mAstern = true; }
                sign = mAsternPanSign;
            } else if (mAstern && std::fabs(bearing) > kAsternExitRad) {
                sign = mAsternPanSign;
            } else {
                mAstern = false;
                mAsternPanSign = sign;
            }
            pan = sign * std::min(std::fabs(bearing) / kFullLeanRad, 1.0f) * kGuideMaxPan;
            mLastBearingDeg = bearing * (180.0f / 3.14159265f);
            mLastHorizonUnits = horizon;
        }
    }
    if (!have) {
        // No trustworthy aim this frame: fade to centre rather than hold a stale lean.
        mLastBearingDeg = 0.0f;
        mLastHorizonUnits = 0.0f;
        mAstern = false;
        mAsternPanSign = 0.0f;
    }
    mPan += (pan - mPan) * alpha;
}

void DriveAssist::Tick(const RaceState& state, const CourseMap& map, const Handedness& handedness,
                       int station, float dtSec) {
    if (!state.valid || !state.driving || !map.Loaded()) {
        mPan = 0.0f;
        mLastBearingDeg = 0.0f;
        mLastHorizonUnits = 0.0f;
        mAstern = false;
        mAsternPanSign = 0.0f;
        return;
    }
    UpdateSteering(state, map, handedness, station, dtSec);
    UpdateCurveCues(state, map, station, dtSec);
}
}  // namespace a11y::race
