#include "accessibility/race/track_limits.h"

#include <algorithm>
#include <cmath>

#include "accessibility/audio/cue_service.h"
#include "accessibility/race/course_map.h"
#include "accessibility/race/heading.h"
#include "accessibility/localization.h"
#include "accessibility/race/race_state.h"
#include "accessibility/screen_reader.h"
#include "runtime_config.h"

namespace a11y::race {
namespace {

using audio::CueChannel;
using audio::CueService;
using audio::CueSpec;
using audio::Waveform;

// Where the approach beeps start, as a fraction of the game's own drivable corridor: 1.0 is the
// corridor edge exactly, so they begin the moment the kart leaves the lane band the CPU drivers
// keep to, and a normal racing line inside it stays quiet. The corridor is narrower than the
// painted road by a course-dependent amount, which is why geometry alone is only the warning: the
// game's own surface signal below is the authority on "actually off the road".
constexpr float kEdgeOnset = 1.0f;

// Releases lower than it engages. Without that, a kart riding the boundary crosses it several
// times a second, and since crossing back resets the beep interval the rate limiter is defeated
// by its own reset - a beep every frame instead of one every 450 ms.
constexpr float kEdgeRelease = 0.9f;

// Perceptual ramp only: how far past the corridor edge the beeps reach full urgency. Where the
// painted road really ends is course-dependent, so urgency simply saturates half a corridor out
// and the held tone takes over once the surface says the road is gone.
constexpr float kEdgeUrgencySpan = 0.5f;

// Each beep re-arms this much further out - a quarter of the corridor per renewed warning
// (perceptual spacing). Drifting OUTWARD keeps warning; holding a stable line outside the CPUs'
// lane goes quiet, because the corridor is the lane, not the road, and a play-test heard "the
// edge" nag continuously while the kart sat on real asphalt the lane does not cover.
constexpr float kEdgeStep = 0.25f;

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

constexpr float kDemoEdgeSec = 0.8f;  // demo: long enough to hear the held tone's timbre clearly
constexpr float kDemoEdgePan = 0.7f;  // demo: el lado del peligro suena a la derecha

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

void TrackLimits::Reset() {
    mBeepTimer = 0.0f;
    mBeepLevel = 0.0f;
    mNearEdge = false;
    mHoldingTone = false;
    mWasOffRoad = false;
    mWasWrongWay = false;
    CueService::Instance().Stop(CueChannel::Edge);
}

void TrackLimits::UpdateEdge(const RaceState& state, const CourseMap& map,
                             const Handedness& handedness, int station, float dtSec) {
    (void)station;
    float offset = 0.0f;
    float nearX = 0.0f, nearZ = 0.0f;
    const bool haveOffset = map.RoadOffset(state.x, state.y, state.z, offset, &nearX, &nearZ);

    // Which EAR the danger is in, in the kart's own frame - the same derivation the steering
    // guide uses, through the same Handedness object. Taking the sign from the route frame
    // instead named the wrong ear whenever the kart pointed more than 90 degrees off the track,
    // which is exactly the spun-out moment the off-road tone plays most.
    float rightX = 0.0f, rightZ = 0.0f;
    handedness.RightVector(state, rightX, rightZ);
    const bool towardsRight =
        haveOffset && ((state.x - nearX) * rightX + (state.z - nearZ) * rightZ) > 0.0f;

    // The held tone means "actually off the road", and the game's own surface multiplier is the
    // authority on that - the AI corridor is narrower than the road, so holding the tone on
    // geometry alone made it sound on nearly every sample of a play-test. Panned to the danger
    // side, the same ear the engine leans to under the player's steer-away convention.
    if (state.offRoad) {
        CueSpec tone;
        tone.shape = Waveform::Triangle;
        tone.frequencyHz = kEdgeHz * kEdgeTonePitch;
        tone.amplitude = kEdgeToneAmplitude;
        if (haveOffset && offset != 0.0f) {
            tone.pan = towardsRight ? kEdgePan : -kEdgePan;
        }
        CueService::Instance().SetSustained(CueChannel::Edge, tone);
        mHoldingTone = true;
        mNearEdge = false;
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

    const float magnitude = std::fabs(offset);

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

    // Brief, panned to the side being left - the same side language as the held tone.
    CueSpec beep;
    beep.shape = Waveform::Triangle;
    beep.frequencyHz = kEdgeHz * (kEdgePitchMin + (kEdgePitchMax - kEdgePitchMin) * nearness);
    beep.amplitude = kEdgeBeepAmplitude;
    beep.pan = towardsRight ? kEdgePan : -kEdgePan;
    beep.durationSec = kEdgeBeepSec;
    CueService::Instance().PlayOneShot(CueChannel::Edge, beep);
}

void TrackLimits::UpdateSurface(const RaceState& state) {
    if (state.offRoad == mWasOffRoad) {
        return;
    }
    mWasOffRoad = state.offRoad;
    // Spoken on the transition only. Speaking a continuous value is what turns narration into
    // spam, and the surface either is or is not slowing the kart.
    ScreenReader::Instance().Speak(loc::Get(state.offRoad ? "off_road" : "on_road"),
                                   /*interrupt=*/false);
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
