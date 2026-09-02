#include <algorithm>
#include <cmath>

#include "accessibility/a11y_log.h"
#include "accessibility/audio/cue_service.h"
#include "accessibility/race/anticipation.h"
#include "accessibility/race/course_map.h"
#include "accessibility/race/edge_map.h"
#include "accessibility/race/heading.h"
#include "accessibility/race/race_state.h"
#include "accessibility/race/track_limits.h"

namespace a11y::race {
namespace {

using audio::CueChannel;
using audio::CueService;
using audio::CueSpec;
using audio::Waveform;

// The scale every edge cue is graded on, whichever geometry produced it: 1.0 is where the warning
// starts and 1.5 is the edge itself. Keeping one scale is what lets the real-road measurement and
// the KMP-corridor fallback share the hysteresis, the rate limit and the urgency ramp below.
constexpr float kEdgeOnset = 1.0f;

// Releases lower than it engages. Without that, a kart riding the boundary crosses it several
// times a second, and since crossing back resets the beep interval the rate limiter is defeated
// by its own reset - a beep every frame instead of one every 450 ms.
constexpr float kEdgeRelease = 0.9f;

// How far past the onset full urgency sits. On the corridor fallback that is half a corridor out;
// on the real road it is the edge itself, since the onset margin is measured to the edge.
constexpr float kEdgeUrgencySpan = 0.5f;

// One perceptual step on that scale - a quarter of the span. It is the unit shared by "the grade
// fell back, re-arm lower" and "the grade jumped a whole step in one frame, beep now".
constexpr float kEdgeStep = 0.25f;

// Past this the beeps have nothing left to add: urgency has already saturated, so every further
// beep is the same beep, and the surface tone owns "the road is gone" from here - except for the
// two edge kinds that tone can never cover (see KindHasNoTone).
constexpr float kEdgeBeepCeiling = kEdgeOnset + kEdgeUrgencySpan;

constexpr float kEdgeHz = 300.0f;
constexpr float kEdgeBeepAmplitude = 0.38f;
constexpr float kEdgeBeepSec = 0.06f;
constexpr float kEdgeToneAmplitude = 0.30f;
constexpr float kEdgePitchMin = 0.8f;
constexpr float kEdgePitchMax = 1.8f;
constexpr float kEdgeTonePitch = 1.8f;
constexpr float kEdgePan = 0.85f;

// One sound for every kind of edge. A drop used to sound an octave down on a saw; the player
// asked for the same sound everywhere (2026-09-02), so the edge kind now only shapes where the
// line is placed, never what the cue sounds like.

// On the edge: the body is touching the boundary and the next hand-width is off the road. The
// beeps' top note an octave up, held - a change of sound at the moment Forza changes its own, so
// "about to cross" is never mistaken for "still approaching".
constexpr float kEdgeOnEdgeOctave = 2.0f;

// Leaving and returning are one short note each, on the beeps' own extremes.
constexpr float kSurfaceChangeSec = 0.15f;

// The yaw estimate is a difference of unit vectors, so what it has to reject is a single-frame
// outlier; three samples is the shortest window that outvotes one.
constexpr float kYawSmoothSamples = 3.0f;

// Below this total turn the constant-yaw arc is its straight limit; in float, 1 - cos loses the
// digits before the arc differs from the chord by a unit at any racing speed.
constexpr float kStraightTurnRad = 1e-3f;

constexpr float kDemoEdgeSec = 0.8f;  // demo: long enough to hear the held tone timbre clearly
constexpr float kDemoEdgePan = 0.7f;  // demo: the danger side sounds to the right

// The held tone needs offRoad && onGround, and neither a wall scrape (still on road) nor a fall
// (airborne) ever satisfies it. For those two the beeps cannot hand over at the ceiling.
bool KindHasNoTone(EdgeKind kind) { return kind == EdgeKind::Wall || kind == EdgeKind::Fall; }

float GradeForMargin(float margin, float onsetMargin) {
    return kEdgeOnset + (1.0f - margin / onsetMargin) * kEdgeUrgencySpan;
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

// Margin from a lateral offset (signed to the track's right) to the edge on one side.
float MarginToSide(float room, float lateral, bool right) {
    return room - (right ? lateral : -lateral);
}

float BeepIntervalSec(float nearness) {
    return kIntervalFarSec + (kIntervalNearSec - kIntervalFarSec) * nearness;
}

// The cue grade on the shared scale. The warning starts at the course's warning distance from the
// edge (EdgeMap::WarningDistance, the same distance the line keeps from a wall or the grass), an
// absolute distance as Forza grades it, so a line placed near a limit is exactly at the edge of
// silence and any drift towards that limit is heard at once. Where the road on that side is
// narrower than the warning distance the band is the whole of it. Without a measured road the
// KMP corridor offset IS the grade, clamped to the same ceiling the measured path saturates at,
// so a failed probe cannot hand the ramp a number from a different scale.
float EdgeMagnitude(float offset, bool haveReal, float realDistance, float margin,
                    float predictedMargin) {
    const float onsetMargin = std::min(EdgeMap::WarningDistance(), realDistance);
    if (!haveReal || !(onsetMargin > 0.0f)) {
        return std::min(std::fabs(offset), kEdgeBeepCeiling);
    }
    // The predicted margin is what the grade rides, which is what makes the lead a TIME. The
    // positional grade stays as the floor, so a kart parked beside the edge still warns.
    return std::max(GradeForMargin(margin, onsetMargin),
                    GradeForMargin(predictedMargin, onsetMargin));
}

}  // namespace

CueSpec SurfaceChangeCue(bool offRoad, bool right) {
    CueSpec note;
    note.shape = Waveform::Triangle;
    note.frequencyHz = kEdgeHz * (offRoad ? kEdgePitchMin : kEdgePitchMax);
    note.amplitude = kEdgeBeepAmplitude;
    note.pan = offRoad ? (right ? kEdgePan : -kEdgePan) : 0.0f;
    note.durationSec = kSurfaceChangeSec;
    return note;
}

void PlayEdgeCueDemo() {
    CueSpec tone;
    tone.shape = Waveform::Triangle;
    tone.frequencyHz = kEdgeHz * kEdgeTonePitch;
    tone.amplitude = kEdgeToneAmplitude;
    tone.pan = kDemoEdgePan;
    tone.durationSec = kDemoEdgeSec;
    CueService::Instance().PlayOneShot(CueChannel::Edge, tone);
}

void TrackLimits::UpdateEdge(const RaceState& state, const CourseMap& map,
                             const Handedness& handedness, int station, float dtSec, bool offRoad) {
    // The kart continuous arc, resolved once: it anchors the lateral offset AND the edge lookup
    // below, so both describe the same point on the same line.
    const float arc = map.ArcOfPosition(state.x, state.z, station);
    float offset = 0.0f;
    float nearX = 0.0f, nearZ = 0.0f, corridor = 0.0f;
    const bool haveOffset =
        map.RoadOffsetAtArc(arc, state.x, state.y, state.z, offset, &nearX, &nearZ, &corridor);

    // Which EAR the danger is in, in the kart own frame - the same derivation the steering guide
    // uses, through the same Handedness object. Taking the sign from the route frame instead named
    // the wrong ear whenever the kart pointed more than 90 degrees off the track.
    float rightX = 0.0f, rightZ = 0.0f;
    handedness.RightVector(state, rightX, rightZ);
    const bool towardsRight =
        haveOffset && ((state.x - nearX) * rightX + (state.z - nearZ) * rightZ) > 0.0f;

    // Yaw from the heading the state already reads: that vector times the speed IS the velocity,
    // so this is the curvature of the path driven, not the chassis yaw a drift throws around.
    if (mHaveLastForward && state.frameSec > 0.0f) {
        const float yawAlpha = 1.0f - std::exp(-1.0f / kYawSmoothSamples);
        const float sinTurn =
            std::clamp(-(mLastForwardX * rightX + mLastForwardZ * rightZ), -1.0f, 1.0f);
        mYawRate += (std::asin(sinTurn) / state.frameSec - mYawRate) * yawAlpha;
    }
    mLastForwardX = state.forwardX;
    mLastForwardZ = state.forwardZ;
    mHaveLastForward = true;

    // WHICH edge is being approached is a question about the track, not about where the kart is
    // pointed, so it takes the route frame own sign while the ear above keeps the kart one. Read at
    // the kart continuous arc, not at its station: a stepped edge distance would jump the onset and
    // the grade every time a station boundary went by.
    bool edgeOnRight = offset > 0.0f;
    float realDistance = 0.0f;
    EdgeKind kind = EdgeKind::Unknown;
    bool haveReal = haveOffset && EdgeMap::SideAtArc(map, arc, edgeOnRight, realDistance, kind);
    const float lateralUnits = offset * corridor;
    float margin = haveReal ? MarginToSide(realDistance, lateralUnits, edgeOnRight) : 0.0f;

    // Where the kart lands one anticipation horizon from now if it keeps its speed and its
    // turning, in the road's frame THERE: the margin it is about to have, on whichever side that
    // is. A straight extrapolation of today's drift misses a road curving away under a kart that
    // is not turning enough - the margin the player reported losing without warning - and a line
    // running along a limit closes on the edge faster than the kart drifts.
    float predictedMargin = margin;
    if (haveOffset) {
        const float seconds = AnticipationSeconds();
        float fx = 0.0f, fz = 0.0f;
        FuturePosition(state, rightX, rightZ, mYawRate, seconds, fx, fz);
        const float ahead = arc + state.speedPerSecond * seconds;
        int aimStation = station;
        float t = 0.0f;
        map.SegmentAtArc(map.LapLength() > 0.0f ? std::fmod(ahead, map.LapLength()) : ahead,
                         aimStation, t);
        const float futureArc = map.ArcOfPosition(fx, fz, aimStation);
        float fOffset = 0.0f, fCorridor = 0.0f;
        float fRoom = 0.0f;
        EdgeKind fKind = EdgeKind::Unknown;
        if (map.RoadOffsetAtArc(futureArc, fx, state.y, fz, fOffset, nullptr, nullptr,
                                &fCorridor)) {
            const float fLateral = fOffset * fCorridor;
            const bool fRight = fLateral > 0.0f;
            if (EdgeMap::SideAtArc(map, futureArc, fRight, fRoom, fKind)) {
                predictedMargin = MarginToSide(fRoom, fLateral, fRight);
                // The side about to be lost is the side that is graded and heard, even when
                // the kart currently sits nearer the other one.
                if (fRight != edgeOnRight && (!haveReal || predictedMargin < margin)) {
                    float roomNow = 0.0f;
                    EdgeKind kindNow = EdgeKind::Unknown;
                    if (EdgeMap::SideAtArc(map, arc, fRight, roomNow, kindNow)) {
                        edgeOnRight = fRight;
                        realDistance = roomNow;
                        kind = kindNow;
                        haveReal = true;
                        margin = MarginToSide(roomNow, lateralUnits, fRight);
                    }
                }
            }
        }
    }
    mNearEdgeKind = haveReal ? kind : EdgeKind::Unknown;
    // The ear the danger is in, in the kart's frame: the present side's ear, mirrored when the
    // graded side is the other one.
    const bool earRight = (offset > 0.0f) == edgeOnRight ? towardsRight : !towardsRight;
    const bool panRight = PannedSideIsRight(earRight, haveOffset, dtSec);

    // How fast the margin on THAT side is shrinking: the kart speed projected on the route
    // across-track axis, signed positive towards the edge being graded. Units per SECOND, which is
    // what turns the lead below into a time instead of a distance that halves with engine class.
    float acrossX = 0.0f, acrossZ = 0.0f;
    map.RightVectorAtArc(arc, acrossX, acrossZ);
    const float closingRate = state.speedPerSecond *
                              (state.forwardX * acrossX + state.forwardZ * acrossZ) *
                              (edgeOnRight ? 1.0f : -1.0f);

    // The held tone means "actually off the road", and the game own surface multiplier is the
    // authority on that - already debounced by the caller. Panned to the danger side, the same ear
    // the engine leans to under the player steer-away convention.
    if (offRoad) {
        CueSpec tone;
        tone.shape = Waveform::Triangle;
        tone.frequencyHz = kEdgeHz * kEdgeTonePitch;
        tone.amplitude = kEdgeToneAmplitude;
        if (haveOffset && offset != 0.0f) {
            tone.pan = panRight ? kEdgePan : -kEdgePan;
        }
        if (!mHoldingTone) {
            // Temporary, with the other cue diagnostics: the margin the beeps actually gave before
            // the road ran out is unmeasurable without this line.
            RT_LOGF(RT_TAG_A11Y, "edge tone ON: offset=%.2f kind=%s\n",
                    static_cast<double>(offset), EdgeKindName(mNearEdgeKind));
        }
        CueService::Instance().SetSustained(CueChannel::Edge, tone);
        mHoldingTone = true;
        mHoldingEdge = false;
        // The approach state is left exactly as it was: clearing it here made every return to the
        // road re-arm an immediate beep. Only a genuine return inside kEdgeRelease disarms it.
        return;
    }
    if (mHoldingTone) {
        CueService::Instance().Stop(CueChannel::Edge);
        mHoldingTone = false;
    }
    if (!haveOffset) {
        mNearEdge = false;
        return;
    }

    // Right on the edge: the body reaches the boundary. Only the measured road can say so, and
    // only with a kart size to compare against.
    const bool onEdge = haveReal && state.bodyHalfWidth > 0.0f && margin <= state.bodyHalfWidth;
    if (onEdge) {
        CueSpec tone;
        tone.shape = Waveform::Triangle;
        tone.frequencyHz = kEdgeHz * kEdgePitchMax * kEdgeOnEdgeOctave;
        tone.amplitude = kEdgeToneAmplitude;
        tone.pan = panRight ? kEdgePan : -kEdgePan;
        if (!mHoldingEdge) {
            RT_LOGF(RT_TAG_A11Y, "edge ON: margin=%.0f body=%.0f kind=%s\n",
                    static_cast<double>(margin), static_cast<double>(state.bodyHalfWidth),
                    EdgeKindName(mNearEdgeKind));
        }
        CueService::Instance().SetSustained(CueChannel::Edge, tone);
        mHoldingEdge = true;
        return;  // the approach state waits underneath, as it does under the held tone
    }
    if (mHoldingEdge) {
        CueService::Instance().Stop(CueChannel::Edge);
        mHoldingEdge = false;
    }
    // The positional grade is the floor; the prediction can only raise it, so moving away never
    // makes the warning louder (EdgeMagnitude takes the max).
    const float magnitude = EdgeMagnitude(offset, haveReal, realDistance, margin, predictedMargin);

    // The two grades agree on their thresholds but not on how fast they move through them, so a
    // switch between them re-arms the limiter and must not read as a surge.
    if (haveReal != mGradeIsReal) {
        mGradeIsReal = haveReal;
        mBeepLevel = kEdgeOnset;
        mLastMagnitude = magnitude;
    }
    // A whole perceptual step crossed inside ONE frame is the emergency no rate limit may swallow.
    // Only meaningful mid-approach; entering the band arms its own immediate beep below.
    const bool surge = mNearEdge && (magnitude - mLastMagnitude) > kEdgeStep;
    mLastMagnitude = magnitude;

    if (mNearEdge) {
        mNearEdge = magnitude >= kEdgeRelease;
        if (!mNearEdge) {
            mBeepLevel = kEdgeOnset;  // back inside the lane: the next approach warns afresh
        }
    } else if (magnitude >= kEdgeOnset) {
        mNearEdge = true;
        mBeepTimer = 0.0f;  // the first beep of an approach is immediate
        mBeepLevel = kEdgeOnset;
    }
    if (!mNearEdge) {
        return;
    }

    const float nearness = std::clamp((magnitude - kEdgeOnset) / kEdgeUrgencySpan, 0.0f, 1.0f);
    const float interval = BeepIntervalSec(nearness);

    // At the ceiling the beeps normally hand over to the surface tone - but that tone never sounds
    // for a wall or a fall, so those two keep beeping at the near interval and saturated pitch
    // until the kart is back inside the release band, instead of going silent at the worst moment.
    const bool saturated = magnitude >= kEdgeBeepCeiling;
    const bool holdAtCeiling = saturated && KindHasNoTone(mNearEdgeKind);
    if (saturated && !holdAtCeiling) {
        return;
    }

    // Coming back toward the lane re-arms the limiter lower, so a renewed drift outward warns
    // again instead of staying armed at the worst excursion of the lap.
    if (magnitude < mBeepLevel - kEdgeStep) {
        mBeepLevel = std::max(kEdgeOnset, magnitude);
    }

    // The hold is re-derived from the CURRENT nearness every frame rather than latched by the beep
    // that armed it: latched far, the whole warning at 200cc was one beep before the road ran out.
    mBeepTimer = std::clamp(mBeepTimer - dtSec, 0.0f, interval);
    if (mBeepTimer > 0.0f && !surge) {
        return;
    }
    // A stable line off the lane stays quiet; a grade that keeps rising keeps beeping.
    if (magnitude < mBeepLevel && !surge && !holdAtCeiling) {
        return;
    }
    mBeepLevel = magnitude;
    mBeepTimer = interval;

    // Brief, panned to the side being left - the same side language as the held tone.
    CueSpec beep;
    beep.shape = Waveform::Triangle;
    beep.frequencyHz = kEdgeHz * (kEdgePitchMin + (kEdgePitchMax - kEdgePitchMin) * nearness);
    beep.amplitude = kEdgeBeepAmplitude;
    beep.pan = panRight ? kEdgePan : -kEdgePan;
    beep.durationSec = kEdgeBeepSec;
    CueService::Instance().PlayOneShot(CueChannel::Edge, beep);
    // Temporary, same reason as the tone log above: the margin left and the margin predicted are
    // what the next lap log calibrates the anticipation horizon against.
    RT_LOGF(RT_TAG_A11Y,
            "edge beep: station=%d kind=%s real=%.0f lateral=%.0f margin=%.0f predicted=%.0f "
            "closing=%.0f grade=%.2f nearness=%.2f side=%s measured=%s\n",
            station, EdgeKindName(mNearEdgeKind), static_cast<double>(realDistance),
            static_cast<double>(lateralUnits), static_cast<double>(margin),
            static_cast<double>(predictedMargin), static_cast<double>(closingRate),
            static_cast<double>(magnitude), static_cast<double>(nearness),
            panRight ? "right" : "left", towardsRight ? "right" : "left");
}

}  // namespace a11y::race
