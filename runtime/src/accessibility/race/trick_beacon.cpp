#include "accessibility/race/trick_beacon.h"

#include "accessibility/audio/cue_service.h"
#include "accessibility/race/beacon_cue.h"
#include "accessibility/race/course_map.h"
#include "accessibility/race/edge_map.h"
#include "accessibility/race/heading.h"
#include "accessibility/race/race_state.h"

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

// The first station of the next run of launch surfaces ahead of `seg`, or -1 when none begins
// within `range` along the line. The run `seg` itself belongs to is skipped: a kart on the ramp
// is past the point of aiming for it.
int NextLaunchStation(const CourseMap& map, float arc, int seg, float range) {
    const int count = map.StationCount();
    bool inRun = EdgeMap::At(seg).launch;
    for (int k = 1; k < count; ++k) {
        const int s = (seg + k) % count;
        if (map.ArcForwardTo(arc, s) > range) {
            return -1;
        }
        const bool launch = EdgeMap::At(s).launch;
        if (launch && !inRun) {
            return s;
        }
        inRun = launch;
    }
    return -1;
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
    CueService::Instance().Stop(CueChannel::TrickRamp);
}

void TrickBeacon::Tick(const RaceState& state, const CourseMap& map, const Handedness& handedness,
                       int station, float dtSec) {
    if (!state.valid || !state.driving || !map.Loaded() || !handedness.Known() ||
        !EdgeMap::Ready()) {
        return;
    }
    mBlipTimer -= dtSec;
    if (mBlipTimer > 0.0f) {
        return;
    }

    const float range = BeaconRange(state.speedPerSecond, map.MedianHalfWidth());
    const float arc = map.ArcOfPosition(state.x, state.z, station);
    int seg = 0;
    float t = 0.0f;
    if (range <= 0.0f || !map.SegmentAtArc(arc, seg, t)) {
        mBlipTimer = 0.0f;
        return;
    }
    const int target = NextLaunchStation(map, arc, seg, range);
    if (target < 0) {
        mBlipTimer = 0.0f;
        return;
    }

    float targetX = 0.0f, targetZ = 0.0f;
    map.Centre(target, targetX, targetZ);
    mBlipTimer = kBeaconIntervalSec;

    // Distance along the line, not as the crow flies: the ramp is reached by following the road.
    const float nearness = BeaconNearness(map.ArcForwardTo(arc, target), range);
    const float amplitude = kTrickVolumeFar + (kTrickVolumeNear - kTrickVolumeFar) * nearness;
    CueService::Instance().PlayOneShot(
        CueChannel::TrickRamp,
        MakeTrickBlip(amplitude, RoadFramePan(map, arc, state, targetX, targetZ)));
}

}  // namespace a11y::race
