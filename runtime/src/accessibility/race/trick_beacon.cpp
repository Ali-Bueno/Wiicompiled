#include "accessibility/race/trick_beacon.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "accessibility/audio/cue_service.h"
#include "accessibility/race/beacon_cue.h"
#include "accessibility/race/course_map.h"
#include "accessibility/race/heading.h"
#include "accessibility/race/kcl_road.h"
#include "accessibility/race/race_state.h"
#include "accessibility/race/ramp_map.h"

namespace a11y::race {
namespace {

using audio::CueChannel;
using audio::CueService;
using audio::CueSpec;
using audio::SampleBank;
using audio::SampleId;
using audio::Waveform;

// A2: one octave below the previous A3 start; player requested a lower pitch, 2026-09-16.
constexpr float kTrickBlipStartHz = 110.0f;
constexpr float kTrickBlipEndHz = 2.0f * kTrickBlipStartHz;
// The longer jump glide chosen after the first 90 ms cut sounded odd, 2026-09-14.
constexpr float kTrickBlipSec = 0.14f;
// A square carries more harmonics than the item box sine, so it sits lower to stay a hint.
constexpr float kTrickVolumeNear = 0.35f;
constexpr float kTrickVolumeFar = 0.12f;

// A ramp too narrow to aim at below one lateral step is still aimed at as if it were that wide.
constexpr float kMinRampHalfWidth = kKclLateralStepUnits;
// The widest heading change a ramp may ask for and still be offered: the bearing the steering guide
// already plays as its whole lean (guide_tuning.h default), a hard turn to a player.
constexpr float kMaxApproachRad = 30.0f * (3.14159265f / 180.0f);

// How far ahead a ramp begins, or a full lap when the kart is already past its start.
float AheadOf(const CourseMap& map, float arc, const Ramp& ramp) {
    return map.ArcBetween(arc, ramp.arcStart);
}

// The heading a ramp asks for: the sideways move onto it over the road left before it begins.
float ApproachRad(const CourseMap& map, float arc, float kartAcross, const Ramp& ramp) {
    const float offset =
        std::fabs(RoadAcross(map, ramp.arcStart, ramp.entryX, ramp.entryZ) - kartAcross);
    return std::atan2(std::max(offset - ramp.halfWidth, 0.0f), AheadOf(map, arc, ramp));
}

bool Reachable(const CourseMap& map, float arc, float kartAcross, float range, const Ramp& ramp) {
    return AheadOf(map, arc, ramp) <= range &&
           ApproachRad(map, arc, kartAcross, ramp) <= kMaxApproachRad;
}

// The next ramp to aim for: the first reachable one within range, or, among those beginning abreast
// of it - within one road half-width along the line - the one asking for the least turn.
int PickRamp(const CourseMap& map, float arc, float kartAcross, float range) {
    const std::vector<Ramp>& ramps = RampMap::Ramps();
    float first = range;
    for (const Ramp& ramp : ramps) {
        if (Reachable(map, arc, kartAcross, range, ramp)) {
            first = std::min(first, AheadOf(map, arc, ramp));
        }
    }
    const float abreast = first + BeaconRoadHalfWidth(map);
    int best = -1;
    float bestApproach = 0.0f;
    for (int i = 0; i < static_cast<int>(ramps.size()); ++i) {
        if (!Reachable(map, arc, kartAcross, range, ramps[i]) ||
            AheadOf(map, arc, ramps[i]) > abreast) {
            continue;
        }
        const float approach = ApproachRad(map, arc, kartAcross, ramps[i]);
        if (best < 0 || approach < bestApproach) {
            best = i;
            bestApproach = approach;
        }
    }
    return best;
}

// Shared by the live Tick blip and the menu demo, so the two constructions cannot drift apart.
CueSpec MakeTrickBlip(float amplitude, float pan) {
    CueSpec blip;
    if (SampleBank::Instance().Has(SampleId::TrickRamp)) {
        blip.sample = SampleId::TrickRamp;
    } else {
        blip.shape = Waveform::Square;
        blip.frequencyHz = kTrickBlipStartHz;
        blip.sweepToHz = kTrickBlipEndHz;
        blip.durationSec = kTrickBlipSec;
    }
    blip.amplitude = amplitude;
    blip.pan = pan;
    return blip;
}

}  // namespace

void PlayTrickRampCueDemo() {
    CueService::Instance().PlayOneShot(CueChannel::TrickRamp,
                                       MakeTrickBlip(kTrickVolumeNear, 0.0f));
}

void TrickBeacon::Reset() {
    mBlipTimer = 0.0f;
    mTarget = -1;
    CueService::Instance().Stop(CueChannel::TrickRamp);
}

void TrickBeacon::Tick(const RaceState& state, const CourseMap& map, const Handedness& handedness,
                       int station, float dtSec) {
    if (!state.valid || !state.driving || !map.Loaded() || !handedness.Known() ||
        !RampMap::Ready()) {
        mTarget = -1;
        return;
    }
    mBlipTimer -= dtSec;
    if (mBlipTimer > 0.0f) {
        return;
    }

    const float range = BeaconRange(state.speedPerSecond, map.MedianHalfWidth());
    const float arc = map.ArcOfPosition(state.x, state.z, station);
    const float kartAcross = RoadAcross(map, arc, state.x, state.z);
    const std::vector<Ramp>& ramps = RampMap::Ramps();
    // Held until passed or out of reach, so two ramps abreast cannot trade places blip by blip.
    if (mTarget < 0 || mTarget >= static_cast<int>(ramps.size()) ||
        !Reachable(map, arc, kartAcross, range, ramps[mTarget])) {
        mTarget = range > 0.0f ? PickRamp(map, arc, kartAcross, range) : -1;
    }
    if (mTarget < 0) {
        mBlipTimer = 0.0f;
        return;
    }
    const Ramp& ramp = ramps[mTarget];
    mBlipTimer = kBeaconIntervalSec;

    // Distance along the line, not as the crow flies: the ramp is reached by following the road.
    const float nearness = BeaconNearness(AheadOf(map, arc, ramp), range);
    const float amplitude = kTrickVolumeFar + (kTrickVolumeNear - kTrickVolumeFar) * nearness;
    // Full pan where the kart's centre would just miss the ramp's edge.
    const float scale = std::max(ramp.halfWidth, kMinRampHalfWidth) + state.bodyHalfWidth;
    CueService::Instance().PlayOneShot(
        CueChannel::TrickRamp,
        MakeTrickBlip(amplitude, RoadFramePan(map, arc, state, ramp.arcStart, ramp.entryX,
                                              ramp.entryZ, scale)));
}

}  // namespace a11y::race
