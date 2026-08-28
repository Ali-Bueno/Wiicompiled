#include "accessibility/race/drive_assist.h"

#include <algorithm>
#include <cmath>

#include "accessibility/a11y_log.h"
#include "accessibility/race/course_map.h"
#include "accessibility/race/edge_map.h"
#include "accessibility/race/heading.h"
#include "accessibility/race/race_state.h"
#include "runtime_config.h"

namespace a11y::race {
namespace {

constexpr float kDegToRad = 3.14159265358979f / 180.0f;

// No centre deadzone, continuous pan (Top Speed's design), smoothed so the lean glides. The
// smoother is a time constant, not a per-frame step: ~100 ms is the 60 fps equivalent of the
// play-tested 0.15-per-frame filter, and expressing it in seconds keeps the feel - and the
// player's by-ear tuning - identical when the frame rate is not 60.
constexpr float kPanSmoothTauSec = 0.1f;

// Nearly dead astern, the aim point's side is numerical noise around the atan2 seam, and each
// flip would glide the pan through zero - the one value that must mean "on the line". Past this
// bearing the guide holds whichever side it already leans to until the kart rotates back.
constexpr float kAsternRad = 150.0f * kDegToRad;

// How far ahead the pursuit point sits, as a TIME. Anticipation is what the player spends to hear
// a cue, decide and move the stick, and that budget is in seconds, not in road lengths - so this
// leads by seconds exactly as the corner calls already do (LeadSeconds, drive_curves.cpp).
//
// It was written as a distance in road widths, which silently shrinks the warning as the course
// gets faster: a logged Retro Rewind session measured `steering_look_ahead = 15` buying 1333 units
// on a 1300-unit half-width, against 6590 units/s at racing pace - 0.20 s, less than the time it
// takes to react to a sound at all, and the top of the knob only reached 0.79 s.
//
// The range is that same widths range converted at that session's own racing speed, so a course
// driven at vanilla pace keeps the feel it was tuned with and only a faster one changes:
//   near  0.5 widths -> 0.5 * 1300 / 6590 = 0.10 s   ("pure present tense")
//   far   4.0 widths -> 4.0 * 1300 / 6590 = 0.79 s   (the "about a second" the design always meant)
constexpr float kAimNearSec = 0.10f;
constexpr float kAimFarSec = 0.79f;

// The floor, still on the course's road scale: a pure time horizon collapses onto the kart's own
// nose at a standstill or after a spin - the exact "no anticipation at all" failure the road-width
// fix just removed - and after a spin the aim point's side IS the recovery signal.
constexpr float kAimNearWidths = 0.5f;

// The bearing to the pursuit point that means full lean: MK64's play-tested 45 degrees, kept
// because the consumer (an ear) is the same. `steering_sensitivity` sweeps a factor of two around
// it - 90 degrees at 0, 22.5 at 100 - so 50 IS the MK64 guide.
constexpr float kFullLeanAnchorRad = 45.0f * kDegToRad;

float Fraction(int percent) {
    return std::clamp(static_cast<float>(percent), 0.0f, 100.0f) / 100.0f;
}

// Top Speed's own pan at the road edge: 25 of the +/-100 DirectSound range (its Car.cpp:1198-1234,
// pan = (relPos - 0.5)^2 * 100, so an edge at relPos 0 or 1 gives 0.5^2 * 100 = 25). Keeping that
// ratio is what makes this feel like the game the player knows: at the default gain the real edge
// leans the engine a quarter of full lean, and the square keeps the centre nearly flat.
constexpr float kTopSpeedEdgeFraction = 0.25f;

// The knob value that reproduces Top Speed's ratio exactly; the knob scales linearly around it, so
// 100 doubles the lean at the edge and 0 turns the term off.
constexpr int kPositionGainDefault = 50;

// The smallest change in the position lean worth a diagnostic line - a twentieth of the pan range,
// which is about the smallest step the ear places at all.
constexpr float kPositionLogStep = 0.05f;

}  // namespace

void DriveAssist::Reset() {
    mSmoothedPan = 0.0f;
    mLastToward = 0.0f;
    mLastPositionBucket = 0;
    mLastPursuitBucket = 0;
    mApproachEntry = -1;
    mLastLateralUnits = 0.0f;
    mLastLap = -1;
    mActiveEntry = -1;
    mApproachBeeps = 0;
    mAnnounced = false;
    mPhase = 0;
    mFinishedEntry = -1;
    mChainAnnounced.clear();
}

// Works out the steering pan and leaves the answer in mSmoothedPan. It plays nothing: the pan is
// applied to the game's own engine sound, which already carries speed in its pitch and loudness,
// so adding a tone of our own would be a second engine over the first.
//
// WHAT THE SIGN MEANS - specified explicitly by the player, 2026-08-27: the engine marks the side
// to steer AWAY from. "Cuando haya curva a la izquierda, el motor se panee a la derecha y
// viceversa - así sé que tengo que regresar el kart al centro para no chocarme." A left-hander
// carries the kart to the right outside, so the engine sits right and coming back to centre is
// steering away from the sound. This is also the one direction language the rest of the mod
// speaks: the edge beeps and the off-road tone already sound on the danger side. The opposite
// preference (steer toward the sound) is `invert_steering_pan`.
//
// Pure pursuit, the model the MK64 mod took from Forza's 2023 Blind Driving Assist and the
// player validated there: aim at a point ON the line a little way ahead, and the pan is the
// signed bearing from the kart's nose to that point. Centre pan therefore MEANS "you are on the
// line, pointing along it" - the player's own definition of the cue. One quantity, so position
// error and heading error can never disagree; and after a spin the aim point is simply behind,
// full lean, and following it turns the kart back down the course - the recovery the previous
// two-term guide never gave (a session log showed 90% of samples off-corridor once the first
// wall was hit).
void DriveAssist::UpdateSteering(const RaceState& state, const CourseMap& map,
                                 const Handedness& handedness, int station, float dtSec) {
    const float alpha = dtSec > 0.0f ? 1.0f - std::exp(-dtSec / kPanSmoothTauSec) : 0.0f;

    // The aim distance rides on the REAL road, the same width the position term grades against, so
    // one setting means the same anticipation on every course. The KMP corridor is not a road
    // width: on a Retro Rewind N64 course it measured 79 units against 1,100 of asphalt, which put
    // the aim point under the kart's nose and left the guide with no anticipation at all.
    const float realHalfWidth = EdgeMap::MedianHalfWidth();
    const float kmpHalfWidth = map.MedianHalfWidth();
    const float widthScale = realHalfWidth > 0.0f  ? realHalfWidth
                             : kmpHalfWidth > 0.0f ? kmpHalfWidth
                                                   : map.MeanSpacing();
    const float lookAhead = Fraction(RuntimeConfigFile::AccessibilitySteeringLookAhead());
    const float aimSeconds = kAimNearSec + (kAimFarSec - kAimNearSec) * lookAhead;
    // Magnitude: the speed carries the sign of travel, and reversing must still aim up the course.
    const float speed = std::fabs(state.speedPerSecond);
    // One anticipation horizon for the whole guide: the pursuit term aims at the point this far
    // along the line, and the position term grades the offset it predicts at that same point.
    const float aimDistance = std::max(kAimNearWidths * widthScale, aimSeconds * speed);
    const float aimWidths = widthScale > 0.0f ? aimDistance / widthScale : 0.0f;

    const float arc = map.ArcOfPosition(state.x, state.z, station);
    float targetX = 0.0f, targetZ = 0.0f;
    float bearing = 0.0f;
    // RouteBased, not Loaded: the checkpoint-midpoint fallback carries progress and corner shape
    // but its midpoints can sit off the road entirely - aiming the engine at them would guide the
    // player into whatever the quads happen to span.
    const bool haveTarget = map.RouteBased() && widthScale > 0.0f &&
                            map.PointAtArc(arc + aimDistance, targetX, targetZ) &&
                            handedness.BearingTo(state, targetX, targetZ, bearing);
    if (!haveTarget) {
        // No trustworthy aim point this frame: fade to centre rather than hold a stale lean.
        mSmoothedPan += (0.0f - mSmoothedPan) * alpha;
        mLastBearingDeg = 0.0f;
        mLastReachWidths = 0.0f;
        return;
    }

    const float strength = Fraction(RuntimeConfigFile::AccessibilitySteeringStrength());
    const float sensitivity = Fraction(RuntimeConfigFile::AccessibilitySteeringSensitivity());
    const float fullLean = kFullLeanAnchorRad * std::pow(2.0f, 1.0f - 2.0f * sensitivity);
    float toward = std::clamp(bearing / fullLean, -1.0f, 1.0f);
    const bool astern = std::fabs(bearing) > kAsternRad;
    if (astern) {
        // The aim point is behind: its side is the recovery signal and nothing else may dilute it.
        toward = mLastToward < 0.0f ? -1.0f : 1.0f;
    } else {
        mLastToward = toward;
    }
    // Negated: `toward` points AT the line, and the player asked for the engine on the side to
    // steer AWAY from ("curva a la izquierda -> motor a la derecha"). `steering_strength` governs
    // this part and only this part, exactly as it did before the position term existed.
    const float pursuitPan = -toward * strength;
    const PositionLean lean =
        astern ? PositionLean{}
               : PositionPan(state, map, handedness, arc, aimDistance, targetX, targetZ);
    const float pan = std::clamp(pursuitPan + lean.pan, -1.0f, 1.0f);
    mSmoothedPan += (pan - mSmoothedPan) * alpha;
    mLastBearingDeg = bearing / kDegToRad;
    mLastReachWidths = aimWidths;

    // Temporary. This term shipped inaudible once because nothing logged what it was worth, and
    // its masking of the pursuit term was only found by mining a race log for the two side by
    // side - so the line carries the whole chain. `drift` is what the offset did that the kart's
    // own crossing does not explain: on a straight, a kart holding its line against a weaving
    // reference route shows a near-zero rate and a non-zero drift. Quantised on EITHER lean, so a
    // pursuit swing against a steady position term still prints. Remove with the other cue
    // diagnostics.
    const float drift = lean.lateral - mLastLateralUnits - lean.rate * dtSec;
    mLastLateralUnits = lean.lateral;
    const int positionBucket = static_cast<int>(lean.pan / kPositionLogStep);
    const int pursuitBucket = static_cast<int>(pursuitPan / kPositionLogStep);
    if (positionBucket != mLastPositionBucket || pursuitBucket != mLastPursuitBucket) {
        mLastPositionBucket = positionBucket;
        mLastPursuitBucket = pursuitBucket;
        const bool cancelling = lean.pan * pursuitPan < 0.0f &&
                                std::fabs(lean.pan) > kPositionLogStep &&
                                std::fabs(pursuitPan) > kPositionLogStep;
        // Three decimals on the pans: the CANCEL flag tests the raw floats against
        // kPositionLogStep, so two decimals printed a "+0.05" that looked like it should not have
        // tripped it.
        RT_LOGF(RT_TAG_A11Y,
                "position pan: lat=%.0f rate=%.0f pred=%.0f drift=%.0f u=%.2f pan=%+.3f "
                "pursuit=%+.3f total=%+.3f%s\n",
                static_cast<double>(lean.lateral), static_cast<double>(lean.rate),
                static_cast<double>(lean.predicted), static_cast<double>(drift),
                static_cast<double>(lean.u), static_cast<double>(lean.pan),
                static_cast<double>(pursuitPan), static_cast<double>(pan),
                cancelling ? " CANCEL" : "");
    }
}

// Top Speed's position term, and only that: its pan is position-pure - pan = sign(d) * d^2 - with
// no curvature anywhere in it. A corner is still felt, because understeer makes the lateral error
// grow at a rate the corner's own severity sets and the square turns that growth into a hard lean.
// The zero stays THE LINE rather than the road midpoint: "centre pan means you are on the line" is
// the player's own definition of the cue and does not move.
//
// Returned in ABSOLUTE pan, to be added after the pursuit part has been scaled. Top Speed's 25 is
// 25 of the full +/-100 hardware pan whatever else is set, and scaling it by the guide's strength
// knob instead made it 0.25 * 0.27 = 0.07 of a pan at the player's own setting - a hairpin that
// "seguía en el centro como si nada".
DriveAssist::PositionLean DriveAssist::PositionPan(const RaceState& state, const CourseMap& map,
                                                  const Handedness& handedness, float arc,
                                                  float aimDistance, float targetX,
                                                  float targetZ) const {
    PositionLean out;
    const int gainKnob = RuntimeConfigFile::AccessibilitySteeringPositionGain();
    float offset = 0.0f, corridor = 0.0f;
    if (gainKnob <= 0 ||
        !map.RoadOffset(state.x, state.y, state.z, offset, nullptr, nullptr, &corridor)) {
        return out;
    }
    out.lateral = offset * corridor;

    // Graded on where the kart's heading is taking it, not on where it stands. A cue the player
    // must hear, decide on and act on is a delayed loop, and a purely proportional term inside one
    // overshoots: by the time the sound centres, the kart still carries the lateral speed that put
    // it there, and it sails past - the player's "me paso de largo y me voy hacia el otro lado".
    //
    // Measured against the line AT THE AIM POINT, never against the tangent at the kart. The first
    // cut projected the drift along that tangent, which is a straight line the road walks away from
    // by its own sagitta: at a 6500 unit aim distance and a moderate 10000 unit radius the omitted
    // curvature term is 2132 units against the 953 the projection kept, so in any real bend the
    // predicted offset came out INVERTED - it read "drifting inside" while the kart was heading for
    // the outside, and cancelled the pursuit term that had it right (2.5% of a race's samples).
    //
    // This is the Stanley decomposition: the pursuit term is the BEARING to the aim point and this
    // is the DISTANCE from it, so the two are components of one vector about one point. They can
    // still disagree - heading error against cross-track error, which is real information - but
    // they can no longer disagree about geometry.
    const float aimArc = arc + aimDistance;
    float aimRightX = 0.0f, aimRightZ = 0.0f;
    map.RightVectorAtArc(aimArc, aimRightX, aimRightZ);
    const float projectedX = state.x + state.forwardX * aimDistance;
    const float projectedZ = state.z + state.forwardZ * aimDistance;
    out.predicted = (projectedX - targetX) * aimRightX + (projectedZ - targetZ) * aimRightZ;

    // Diagnostic only: the instantaneous rate at which the kart crosses the line under its nose.
    // The predictor needs no time and no speed - both cancel into the aim distance.
    float trackRightX = 0.0f, trackRightZ = 0.0f;
    map.RightVectorAtArc(arc, trackRightX, trackRightZ);
    out.rate = state.speedPerSecond * (state.forwardX * trackRightX + state.forwardZ * trackRightZ);

    // Side and ear flip TOGETHER with the prediction. Keying the ear off the present offset while
    // the magnitude came from the predicted one would put the loudest warning in the wrong ear at
    // exactly the moment the prediction crossed the line, which is the moment it exists for.
    float kartRightX = 0.0f, kartRightZ = 0.0f;
    handedness.RightVector(state, kartRightX, kartRightZ);
    const bool predictedRight = out.predicted > 0.0f;
    // Two frames, as in the edge cue: WHICH edge is approached is a question about the track, so it
    // takes the route frame's sign at the aim point - the frame the prediction is expressed in;
    // which EAR the lean belongs in is a question about the kart, so it goes through Handedness and
    // stays right when the kart is spun.
    const bool earRight =
        out.predicted * (aimRightX * kartRightX + aimRightZ * kartRightZ) > 0.0f;

    // Normalised by the REAL road where the prediction LANDS, and never by the KMP corridor, which
    // measured 2-4x narrower than the asphalt and would saturate this term while the player is
    // still on track. If that side was never measured there, the side the kart is on at its own
    // arc still gives a road width to grade against - a scale from next door beats no cue at all.
    float edgeDistance = 0.0f;
    EdgeKind kind = EdgeKind::Unknown;
    if (!EdgeMap::SideAtArc(map, aimArc, predictedRight, edgeDistance, kind) &&
        !EdgeMap::SideAtArc(map, arc, offset > 0.0f, edgeDistance, kind)) {
        return out;  // neither side measured here: pure pursuit beats a wrong scale
    }
    // Capped at the edge, because Top Speed's relPos is a fraction of the road and is bounded by
    // construction. Unbounded it squares past 4 and saturates the lean on its own, erasing the
    // pursuit bearing - and it does that precisely when the kart is off the road and the bearing
    // is the one thing that still points the way back.
    out.u = std::min(std::fabs(out.predicted) / edgeDistance, 1.0f);
    const float gain = static_cast<float>(gainKnob) / static_cast<float>(kPositionGainDefault);
    // ON the side the kart is heading for - the engine marks the side to steer AWAY from. Same
    // convention as the pursuit part, one step further along: this is already pan, where that one
    // is still `toward` and gets negated below.
    out.pan = (earRight ? 1.0f : -1.0f) * out.u * out.u * kTopSpeedEdgeFraction * gain;
    return out;
}

void DriveAssist::Tick(const RaceState& state, const CourseMap& map, const Handedness& handedness,
                       int station, float dtSec) {
    if (!state.valid || !state.driving || !map.Loaded()) {
        mSmoothedPan = 0.0f;
        return;
    }

    // Re-armed every lap, not only when the upcoming curve changes. On a course with a single
    // corner - or an oval, which is one continuous curve - the curve ahead never changes, so
    // without this the approach and traversal cues would fire on lap one and stay silent forever.
    if (state.lap != mLastLap) {
        mLastLap = state.lap;
        mActiveEntry = -1;        // every corner is worth calling again on the next lap
        mFinishedEntry = -1;      // including the one the last lap ended inside
        mChainAnnounced.clear();  // including the ones spoken inside a chained call
    }

    UpdateSteering(state, map, handedness, station, dtSec);
    UpdateCurveCues(state, map, station);
}

}  // namespace a11y::race
