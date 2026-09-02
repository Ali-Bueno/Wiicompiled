#include "accessibility/race/drive_assist.h"

#include <algorithm>
#include <cmath>

#include "accessibility/a11y_log.h"
#include "accessibility/race/anticipation.h"
#include "accessibility/race/course_map.h"
#include "accessibility/race/edge_map.h"
#include "accessibility/race/heading.h"
#include "accessibility/race/race_state.h"
#include "runtime_config.h"

namespace a11y::race {
namespace {

// Continuous pan, smoothed so the lean glides: ~100 ms is the 60 fps equivalent of the play-tested
// 0.15-per-frame filter, kept as a time so the feel survives any frame rate.
constexpr float kPanSmoothTauSec = 0.1f;

// The bearing that is the whole lean: 30 degrees, Forza's own at-speed threshold for "way off the
// line" (`assistfocuscarangletolookahead_minpointspeedramped`: 30 degrees once moving, 90 at a
// standstill). A right angle was tried first and left a 400-unit drift off the line at a pan of
// 0.05 - inaudible against a 100-unit margin (2026-09-02 log). The scale is an angle and not the
// road so that the lean still grades a corner: a normal corner's aim point is about here, a
// gentle bend's halfway, a drift on a straight a few degrees.
constexpr float kFullLeanRad = 30.0f * (3.14159265f / 180.0f);

// The smallest change in pan worth a diagnostic line - a twentieth of the range, about the smallest
// step the ear places at all.
constexpr float kLogStep = 0.05f;

float Fraction(int percent) {
    return std::clamp(static_cast<float>(percent), 0.0f, 100.0f) / 100.0f;
}

float WrapArc(float arc, float lapLength) {
    if (lapLength <= 0.0f) {
        return arc;
    }
    arc = std::fmod(arc, lapLength);
    return arc < 0.0f ? arc + lapLength : arc;
}

}  // namespace

void DriveAssist::Reset() {
    mSmoothedPan = 0.0f;
    mLastBearingDeg = 0.0f;
    mLastHorizonUnits = 0.0f;
    mLastLogBucket = 0;
    mLastLap = -1;
    mActiveEntry = -1;
    mApproachEntry = -1;
    mApproachBeeps = 0;
    mAnnounced = false;
    mPhase = 0;
    mFinishedEntry = -1;
    mChainAnnounced.clear();
    mChainGap = 0.0f;
}

// Forza's steering guide, as its own configuration and its players describe it: the bearing
// from the kart to an aim point on the racing line one anticipation horizon ahead, and nothing
// else. A corner is heard as the aim point swings into it - earlier and harder the tighter the
// corner - and a bend driven on the line keeps a steady lean the size of the bend: "keep turning
// this much". Two attempts to make that lean read as centred were played and withdrawn the same
// day (2026-09-02): subtracting the kart's own yaw put the player's steering in the signal at
// 22.6 degrees per rad/s and the pan zigzagged with every correction; subtracting the line's bend
// under the kart went quiet exactly when a kart entered a bend without turning, so the player
// turned late and overshot. The bare bearing has neither defect.
//
// Sign, specified by the player (2026-08-27): the engine marks the side the kart is heading for -
// "curva a la izquierda -> motor a la derecha" - so steering away from the sound is the fix. The
// aim point on the left means the kart is heading for the right of it. The edge beeps and the
// off-road tone speak the same language. `invert_steering_pan` flips it.
void DriveAssist::UpdateSteering(const RaceState& state, const CourseMap& map,
                                 const Handedness& handedness, int station, float dtSec) {
    const float alpha = dtSec > 0.0f ? 1.0f - std::exp(-dtSec / kPanSmoothTauSec) : 0.0f;
    const float strength = Fraction(RuntimeConfigFile::AccessibilitySteeringStrength());
    const float arc = map.ArcOfPosition(state.x, state.z, station);
    float kartRightX = 0.0f, kartRightZ = 0.0f;
    handedness.RightVector(state, kartRightX, kartRightZ);

    float pan = 0.0f;
    // RouteBased, not Loaded: the checkpoint-midpoint fallback carries progress and corner shape
    // but its midpoints can sit off the road entirely.
    bool have = map.RouteBased() && handedness.Known();
    if (have) {
        // Seconds ahead at the current speed, floored at one real road half-width so a kart
        // stopped or spun still aims at something in front of it - after a spin the guide is the
        // recovery signal, not a silence.
        float floorUnits = EdgeMap::MedianHalfWidth();
        if (floorUnits <= 0.0f) {
            floorUnits = map.MedianHalfWidth();
        }
        const float horizon = std::max(state.speedPerSecond * AnticipationSeconds(), floorUnits);
        float aimX = 0.0f, aimZ = 0.0f;
        have = map.PointAtArc(WrapArc(arc + horizon, map.LapLength()), aimX, aimZ);
        if (have) {
            const float dx = aimX - state.x;
            const float dz = aimZ - state.z;
            const float ahead = dx * state.forwardX + dz * state.forwardZ;
            const float right = dx * kartRightX + dz * kartRightZ;
            // Positive with the aim point on the kart's right.
            const float bearing = std::atan2(right, ahead);
            const float lean = std::min(std::fabs(bearing) / kFullLeanRad, 1.0f);
            // Aim point on the right: the kart is heading for the left of the line.
            pan = (bearing > 0.0f ? -1.0f : 1.0f) * lean * strength;
            mLastBearingDeg = bearing * (180.0f / 3.14159265f);
            mLastHorizonUnits = horizon;
        }
    }
    if (!have) {
        // No trustworthy aim this frame: fade to centre rather than hold a stale lean.
        mLastBearingDeg = 0.0f;
        mLastHorizonUnits = 0.0f;
    }
    mSmoothedPan += (pan - mSmoothedPan) * alpha;

    // Temporary: the guide's chain in one line, printed when the pan moves a step. Remove with the
    // other cue diagnostics once the feel is settled.
    const int bucket = static_cast<int>(pan / kLogStep);
    if (bucket != mLastLogBucket) {
        mLastLogBucket = bucket;
        float offsetNow = 0.0f, corridorNow = 0.0f;
        map.RoadOffsetAtArc(arc, state.x, state.y, state.z, offsetNow, nullptr, nullptr,
                            &corridorNow);
        RT_LOGF(RT_TAG_A11Y, "guide: lat=%.0f bearing=%+.1f horizon=%.0f pan=%+.3f heard=%+.3f\n",
                static_cast<double>(offsetNow * corridorNow), static_cast<double>(mLastBearingDeg),
                static_cast<double>(mLastHorizonUnits), static_cast<double>(pan),
                static_cast<double>(mSmoothedPan));
    }
}

void DriveAssist::Tick(const RaceState& state, const CourseMap& map, const Handedness& handedness,
                       int station, float dtSec) {
    if (!state.valid || !state.driving || !map.Loaded()) {
        mSmoothedPan = 0.0f;
        return;
    }

    // Re-armed every lap: on an oval the curve ahead never changes, and without this the corner
    // cues would fire on lap one and stay silent forever.
    if (state.lap != mLastLap) {
        mLastLap = state.lap;
        mActiveEntry = -1;
        mFinishedEntry = -1;
        mChainAnnounced.clear();
    }

    UpdateSteering(state, map, handedness, station, dtSec);
    UpdateCurveCues(state, map, station);
}

}  // namespace a11y::race
