#include <algorithm>
#include <cmath>

#include "accessibility/a11y_log.h"
#include "accessibility/audio/cue_service.h"
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
// the KMP-corridor fallback share the hysteresis, the ratchet and the urgency ramp below.
constexpr float kEdgeOnset = 1.0f;

// Releases lower than it engages. Without that, a kart riding the boundary crosses it several
// times a second, and since crossing back resets the beep interval the rate limiter is defeated
// by its own reset - a beep every frame instead of one every 450 ms.
constexpr float kEdgeRelease = 0.9f;

// How far past the onset full urgency sits. On the corridor fallback that is half a corridor out;
// on the real road it is the edge itself, since the onset margin is measured to the edge.
constexpr float kEdgeUrgencySpan = 0.5f;

// Each beep re-arms this much further out - a quarter of the span per renewed warning (perceptual
// spacing). Drifting OUTWARD keeps warning; holding a stable line goes quiet.
constexpr float kEdgeStep = 0.25f;

// Past this the beeps have nothing left to add: urgency has already saturated, so every further
// beep is the same beep, and the surface tone owns "the road is gone" from here. A real lap logged
// seven identical beeps at offsets of 7 to 8.65 corridors, all at full urgency.
constexpr float kEdgeBeepCeiling = kEdgeOnset + kEdgeUrgencySpan;

// Where the warning starts, as a share of the real road left on that side. The primary limit is the
// game's own authored corridor half-width at that station - what the CPU drivers keep clear of
// trouble - and this caps it so that on a road narrower than two corridors the quiet band is never
// squeezed below the half the player is actually driving in.
constexpr float kEdgeOnsetRealFraction = 0.5f;

constexpr float kEdgeHz = 300.0f;
constexpr float kEdgeBeepAmplitude = 0.38f;
constexpr float kEdgeBeepSec = 0.06f;
constexpr float kEdgeToneAmplitude = 0.30f;
constexpr float kEdgePitchMin = 0.8f;
constexpr float kEdgePitchMax = 1.8f;
constexpr float kEdgeTonePitch = 1.8f;
constexpr float kEdgePan = 0.85f;

// The beeps close up as the edge nears, which is the urgency signal.
constexpr float kIntervalFarSec = 0.45f;
constexpr float kIntervalNearSec = 0.09f;

// Surface flap guard. The game's own off-road multiplier flickers frame to frame where the kart
// rides a boundary or bounces over it, and each flicker restarted the held tone and re-armed an
// immediate beep, defeating the interval limiter. One beep interval at full urgency is the
// shortest gap this cue family already treats as two separate events, so anything shorter is flap
// rather than a change of surface.
constexpr float kEdgeSurfaceDebounceSec = kIntervalNearSec;

// The nearest route segment can jump to a different part of the course - the far side of one of
// Mushroom Gorge's gaps, the other level of a crossover - and the offset's sign jumps with it: a
// real lap logged -4.68 to +6.89 inside one beep interval. A kart cannot cross the line that fast,
// so the panned side only follows a new sign once it has held for longer than such a jump lasts.
constexpr float kEdgeSideHoldSec = kIntervalNearSec * 2.0f;

// A drop sounds an octave under grass, on a saw rather than a triangle. An octave is the clearest
// pitch relation there is, so "vas a caer" and "hierba" can never be taken for one another, and
// urgency still rides the same pitch ramp and the same closing interval inside each family.
constexpr float kEdgeFallOctave = 0.5f;

constexpr float kDemoEdgeSec = 0.8f;  // demo: long enough to hear the held tone's timbre clearly
constexpr float kDemoEdgePan = 0.7f;  // demo: el lado del peligro suena a la derecha

bool KindIsFall(EdgeKind kind) { return kind == EdgeKind::Fall; }

// The cue's grade for the kart's position, on the shared scale. `realDistance` is the road left on
// the side the kart is drifting towards and `corridor` the station's authored half-width; without
// a measured road the corridor offset IS the grade, which is the behaviour before the KCL edges.
//
// The fallback is clamped to the same ceiling the measured path saturates at. Raw, it reaches 7-8
// on a corridor the kart is far outside, so a station whose probe failed would hand the ratchet and
// the pitch ramp a number from a different scale and the cue would change character mid-drift.
float EdgeMagnitude(float lateralUnits, float offset, bool haveReal, float realDistance,
                    float corridor) {
    const float fallback = std::min(std::fabs(offset), kEdgeBeepCeiling);
    if (!haveReal) {
        return fallback;
    }
    const float margin = realDistance - std::fabs(lateralUnits);
    const float onsetMargin = std::min(corridor, kEdgeOnsetRealFraction * realDistance);
    if (!(onsetMargin > 0.0f)) {
        return fallback;
    }
    return kEdgeOnset + (1.0f - margin / onsetMargin) * kEdgeUrgencySpan;
}

}  // namespace

void PlayEdgeCueDemo() {
    CueSpec tone;
    tone.shape = Waveform::Triangle;
    tone.frequencyHz = kEdgeHz * kEdgeTonePitch;
    tone.amplitude = kEdgeToneAmplitude;
    tone.pan = kDemoEdgePan;
    tone.durationSec = kDemoEdgeSec;
    CueService::Instance().PlayOneShot(CueChannel::Edge, tone);
}

// The surface flag has to read the same way for a debounce window before the tone starts, and again
// before it stops, so a boundary flicker cannot chop the tone into a warble.
bool TrackLimits::SurfaceSaysOffRoad(bool offRoad, float dtSec) {
    if (offRoad == mHoldingTone) {
        mSurfaceHoldSec = 0.0f;
        return mHoldingTone;
    }
    mSurfaceHoldSec += dtSec;
    if (mSurfaceHoldSec < kEdgeSurfaceDebounceSec) {
        return mHoldingTone;
    }
    mSurfaceHoldSec = 0.0f;
    return offRoad;
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

void TrackLimits::UpdateEdge(const RaceState& state, const CourseMap& map,
                             const Handedness& handedness, int station, float dtSec) {
    float offset = 0.0f;
    float nearX = 0.0f, nearZ = 0.0f, corridor = 0.0f;
    const bool haveOffset =
        map.RoadOffset(state.x, state.y, state.z, offset, &nearX, &nearZ, &corridor);

    // Which EAR the danger is in, in the kart's own frame - the same derivation the steering
    // guide uses, through the same Handedness object. Taking the sign from the route frame
    // instead named the wrong ear whenever the kart pointed more than 90 degrees off the track,
    // which is exactly the spun-out moment the off-road tone plays most.
    float rightX = 0.0f, rightZ = 0.0f;
    handedness.RightVector(state, rightX, rightZ);
    const bool towardsRight =
        haveOffset && ((state.x - nearX) * rightX + (state.z - nearZ) * rightZ) > 0.0f;
    const bool panRight = PannedSideIsRight(towardsRight, haveOffset, dtSec);

    // WHICH edge is being approached is a question about the track, not about where the kart is
    // pointed, so it takes the route frame's own sign while the ear above keeps the kart's. Read
    // at the kart's continuous arc, not at its station: a stepped edge distance would jump the
    // onset and the grade every time a station boundary went by.
    const float arc = map.ArcOfPosition(state.x, state.z, station);
    float realDistance = 0.0f;
    EdgeKind kind = EdgeKind::Unknown;
    const bool haveReal =
        haveOffset && EdgeMap::SideAtArc(map, arc, offset > 0.0f, realDistance, kind);
    mNearEdgeKind = haveReal ? kind : EdgeKind::Unknown;

    // The held tone means "actually off the road", and the game's own surface multiplier is the
    // authority on that - the AI corridor is narrower than the road, so holding the tone on
    // geometry alone made it sound on nearly every sample of a play-test. Panned to the danger
    // side, the same ear the engine leans to under the player's steer-away convention.
    if (SurfaceSaysOffRoad(state.offRoad, dtSec)) {
        CueSpec tone;
        tone.shape = KindIsFall(mNearEdgeKind) ? Waveform::Saw : Waveform::Triangle;
        tone.frequencyHz =
            kEdgeHz * kEdgeTonePitch * (KindIsFall(mNearEdgeKind) ? kEdgeFallOctave : 1.0f);
        tone.amplitude = kEdgeToneAmplitude;
        if (haveOffset && offset != 0.0f) {
            tone.pan = panRight ? kEdgePan : -kEdgePan;
        }
        if (!mHoldingTone) {
            // Temporary. A real lap's log could not say whether any beep preceded the off-road
            // tone - the margin the edge cue actually gives is unmeasurable without this. Remove
            // with the other cue diagnostics once the KCL-backed edges are calibrated.
            RT_LOGF(RT_TAG_A11Y, "edge tone ON: offset=%.2f kind=%s\n",
                    static_cast<double>(offset), EdgeKindName(mNearEdgeKind));
        }
        CueService::Instance().SetSustained(CueChannel::Edge, tone);
        mHoldingTone = true;
        // The approach state is left exactly as it was: clearing it here made every return to the
        // road re-arm an immediate beep, so a kart bouncing over a boundary beeped every other
        // frame instead of once per interval. Only a genuine return inside kEdgeRelease disarms it.
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

    const float lateralUnits = offset * corridor;
    const float magnitude =
        EdgeMagnitude(lateralUnits, offset, haveReal, realDistance, corridor);

    // The two grades agree on their thresholds but not on how fast they move through them, so a
    // switch between them re-arms the ratchet: carrying a level set by the other measurement over
    // would either hold the cue silent or fire it at once.
    if (haveReal != mGradeIsReal) {
        mGradeIsReal = haveReal;
        mBeepLevel = kEdgeOnset;
    }

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

    // Coming back toward the lane re-arms the ratchet lower, so a renewed drift outward warns
    // again instead of staying armed at the worst excursion of the lap.
    if (magnitude < mBeepLevel - kEdgeStep) {
        mBeepLevel = std::max(kEdgeOnset, magnitude + kEdgeStep);
    }

    if (magnitude >= kEdgeBeepCeiling) {
        return;  // saturated: the surface tone says the rest, and repeating this one says nothing
    }

    const float nearness = std::clamp((magnitude - kEdgeOnset) / kEdgeUrgencySpan, 0.0f, 1.0f);

    mBeepTimer -= dtSec;
    if (mBeepTimer > 0.0f) {
        return;
    }
    if (magnitude < mBeepLevel) {
        return;  // not getting closer to the edge - a stable line off the lane stays quiet
    }
    mBeepLevel = magnitude + kEdgeStep;
    mBeepTimer = kIntervalFarSec + (kIntervalNearSec - kIntervalFarSec) * nearness;

    // Brief, panned to the side being left - the same side language as the held tone. A drop and a
    // grass shoulder are two different sounds, because they are two different mistakes.
    const bool fall = KindIsFall(mNearEdgeKind);
    CueSpec beep;
    beep.shape = fall ? Waveform::Saw : Waveform::Triangle;
    beep.frequencyHz = kEdgeHz * (kEdgePitchMin + (kEdgePitchMax - kEdgePitchMin) * nearness) *
                       (fall ? kEdgeFallOctave : 1.0f);
    beep.amplitude = kEdgeBeepAmplitude;
    beep.pan = panRight ? kEdgePan : -kEdgePan;
    beep.durationSec = kEdgeBeepSec;
    CueService::Instance().PlayOneShot(CueChannel::Edge, beep);
    // Temporary, same reason as the tone log above: the real edge distance and the margin left are
    // what the next lap's log calibrates kEdgeOnsetRealFraction against.
    RT_LOGF(RT_TAG_A11Y,
            "edge beep: station=%d kind=%s real=%.0f lateral=%.0f margin=%.0f grade=%.2f "
            "nearness=%.2f side=%s measured=%s\n",
            station, EdgeKindName(mNearEdgeKind), static_cast<double>(realDistance),
            static_cast<double>(lateralUnits),
            static_cast<double>(realDistance - std::fabs(lateralUnits)),
            static_cast<double>(magnitude), static_cast<double>(nearness),
            panRight ? "right" : "left", towardsRight ? "right" : "left");
}

}  // namespace a11y::race
