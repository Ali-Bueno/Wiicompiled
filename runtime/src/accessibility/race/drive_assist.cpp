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

// The yaw estimate is a difference of unit vectors, so what it has to reject is a single-frame
// outlier; three samples is the shortest window that outvotes one.
constexpr float kYawSmoothSamples = 3.0f;

// Silent centre: a prediction inside this share of the real half-width is "fine, keep going". Half
// the edge cue's own onset band, so the guide always speaks before the edge cue does.
constexpr float kQuietFraction = kEdgeOnsetRealFraction * 0.5f;

// Below this total turn the constant-yaw arc is its straight limit; in float, 1 - cos loses the
// digits before the arc differs from the chord by a unit at any racing speed.
constexpr float kStraightTurnRad = 1e-3f;

// The smallest change in pan worth a diagnostic line - a twentieth of the range, about the smallest
// step the ear places at all.
constexpr float kLogStep = 0.05f;

float Fraction(int percent) {
    return std::clamp(static_cast<float>(percent), 0.0f, 100.0f) / 100.0f;
}

// Where the kart will be after `seconds` if it keeps its speed and its yaw rate: a circular arc,
// or its straight limit. Speed carries the sign of travel, so reversing predicts behind.
void FuturePosition(const RaceState& state, float rightX, float rightZ, float yawRate,
                    float seconds, float& x, float& z) {
    const float distance = state.speedPerSecond * seconds;
    const float turn = yawRate * seconds;
    float along = distance;
    float side = 0.0f;
    if (std::fabs(turn) >= kStraightTurnRad) {
        const float radius = distance / turn;
        along = radius * std::sin(turn);
        side = radius * (1.0f - std::cos(turn));
    }
    x = state.x + state.forwardX * along + rightX * side;
    z = state.z + state.forwardZ * along + rightZ * side;
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
    mYawRate = 0.0f;
    mLastForwardX = 0.0f;
    mLastForwardZ = 0.0f;
    mHaveLastForward = false;
    mLastPanSign = 0.0f;
    mLastPredictedFraction = 0.0f;
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
}

// One quantity: the kart's lateral offset FROM THE LINE at the point it reaches one anticipation
// horizon from now, if it keeps its speed and its turning, as a share of the real road on that
// side. Zero means "you will be on the line" - on a straight pointing along it, or in a bend turning
// at the bend's own rate. A corner ahead of a kart going straight shows up a full horizon early; a
// kart correcting back too hard shows up on the far side BEFORE it crosses, which is what tells the
// player when to stop correcting.
//
// Sign, specified by the player (2026-08-27): the engine marks the side the kart is heading for -
// "curva a la izquierda -> motor a la derecha" - so steering away from the sound is the fix. The
// edge beeps and the off-road tone speak the same language. `invert_steering_pan` flips it.
void DriveAssist::UpdateSteering(const RaceState& state, const CourseMap& map,
                                 const Handedness& handedness, int station, float dtSec) {
    const float alpha = dtSec > 0.0f ? 1.0f - std::exp(-dtSec / kPanSmoothTauSec) : 0.0f;
    const float yawAlpha = 1.0f - std::exp(-1.0f / kYawSmoothSamples);

    // Yaw from the heading the state already reads: that vector times the speed IS the velocity,
    // so this is the curvature of the path driven, not the chassis yaw a drift throws around.
    float kartRightX = 0.0f, kartRightZ = 0.0f;
    handedness.RightVector(state, kartRightX, kartRightZ);
    if (mHaveLastForward && state.frameSec > 0.0f) {
        const float sinTurn =
            std::clamp(-(mLastForwardX * kartRightX + mLastForwardZ * kartRightZ), -1.0f, 1.0f);
        mYawRate += (std::asin(sinTurn) / state.frameSec - mYawRate) * yawAlpha;
    }
    mLastForwardX = state.forwardX;
    mLastForwardZ = state.forwardZ;
    mHaveLastForward = true;

    const float strength = Fraction(RuntimeConfigFile::AccessibilitySteeringStrength());
    const float seconds = AnticipationSeconds();
    const float arc = map.ArcOfPosition(state.x, state.z, station);

    float pan = 0.0f;
    float predicted = 0.0f;
    float edge = 0.0f;
    // RouteBased, not Loaded: the checkpoint-midpoint fallback carries progress and corner shape
    // but its midpoints can sit off the road entirely.
    bool have = map.RouteBased() && handedness.Known();
    if (have) {
        float roadFx = 0.0f, roadFz = 0.0f;
        map.ForwardAtArc(arc, roadFx, roadFz);
        const float along = roadFx * state.forwardX + roadFz * state.forwardZ;
        if (along <= 0.0f) {
            // Facing across or away from the course: no prediction means anything, and the last
            // side leaned to is the recovery signal - hold it at full lean until the kart rotates back.
            pan = mLastPanSign * strength;
        } else {
            const float horizon = state.speedPerSecond * seconds;
            float futureX = 0.0f, futureZ = 0.0f;
            FuturePosition(state, kartRightX, kartRightZ, mYawRate, seconds, futureX, futureZ);
            // Measured in the road's frame where the kart actually lands, so a bend driven at its
            // own rate predicts zero at any curvature.
            int aimStation = station;
            float t = 0.0f;
            map.SegmentAtArc(WrapArc(arc + horizon, map.LapLength()), aimStation, t);
            const float futureArc = map.ArcOfPosition(futureX, futureZ, aimStation);
            float offset = 0.0f, corridor = 0.0f;
            have = map.RoadOffsetAtArc(futureArc, futureX, state.y, futureZ, offset, nullptr,
                                       nullptr, &corridor);
            if (have) {
                predicted = offset * corridor;
                // Normalised by the REAL road on the side being approached, never by the KMP
                // corridor, which measures 2-4x narrower than the asphalt.
                EdgeKind kind = EdgeKind::Unknown;
                if (!EdgeMap::SideAtArc(map, futureArc, predicted > 0.0f, edge, kind) &&
                    !EdgeMap::SideAtArc(map, arc, predicted > 0.0f, edge, kind)) {
                    edge = EdgeMap::MedianHalfWidth();
                }
                have = edge > 0.0f;
            }
            if (have) {
                const float fraction = std::fabs(predicted) / edge;
                const float live =
                    std::min((std::max(fraction - kQuietFraction, 0.0f)) / (1.0f - kQuietFraction),
                             1.0f);
                // The side is a question about the track, the ear a question about the kart; with
                // the kart facing along the course (`along > 0`) the two agree up to a sign.
                float trackRightX = 0.0f, trackRightZ = 0.0f;
                map.RightVectorAtArc(futureArc, trackRightX, trackRightZ);
                const bool earRight =
                    predicted * (trackRightX * kartRightX + trackRightZ * kartRightZ) > 0.0f;
                pan = (earRight ? 1.0f : -1.0f) * live * strength;
                if (live > 0.0f) {
                    mLastPanSign = earRight ? 1.0f : -1.0f;
                }
                mLastPredictedFraction = predicted / edge;
                mLastHorizonUnits = horizon;
            }
        }
    }
    if (!have) {
        // No trustworthy prediction this frame: fade to centre rather than hold a stale lean.
        mLastPredictedFraction = 0.0f;
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
        RT_LOGF(RT_TAG_A11Y,
                "guide: lat=%.0f pred=%.0f edge=%.0f frac=%+.2f yaw=%+.2f horizon=%.0f "
                "pan=%+.3f heard=%+.3f\n",
                static_cast<double>(offsetNow * corridorNow), static_cast<double>(predicted),
                static_cast<double>(edge), static_cast<double>(mLastPredictedFraction),
                static_cast<double>(mYawRate), static_cast<double>(mLastHorizonUnits),
                static_cast<double>(pan), static_cast<double>(mSmoothedPan));
    }
}

void DriveAssist::Tick(const RaceState& state, const CourseMap& map, const Handedness& handedness,
                       int station, float dtSec) {
    if (!state.valid || !state.driving || !map.Loaded()) {
        mSmoothedPan = 0.0f;
        mHaveLastForward = false;
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
