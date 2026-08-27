#include "accessibility/race/drive_assist.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "accessibility/a11y_log.h"
#include "accessibility/audio/cue_service.h"
#include "accessibility/race/course_map.h"
#include "accessibility/race/heading.h"
#include "accessibility/localization.h"
#include "accessibility/race/race_state.h"
#include "runtime_config.h"
#include "accessibility/screen_reader.h"

namespace a11y::race {
namespace {

using audio::CueChannel;
using audio::CueService;
using audio::CueSpec;
using audio::Waveform;

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

// How far ahead the pursuit point sits, in units of the lap's median corridor half-width - the
// course's own road scale, so a setting means the same thing on every course and on either line
// source. `steering_look_ahead` sweeps the range: at 0 the aim is half a half-width out, pure
// present tense; at 100 it is four, about a second of anticipation at racing speed.
constexpr float kAimNearWidths = 0.5f;
constexpr float kAimFarWidths = 4.0f;

// The bearing to the pursuit point that means full lean: MK64's play-tested 45 degrees, kept
// because the consumer (an ear) is the same. `steering_sensitivity` sweeps a factor of two around
// it - 90 degrees at 0, 22.5 at 100 - so 50 IS the MK64 guide.
constexpr float kFullLeanAnchorRad = 45.0f * kDegToRad;

float Fraction(int percent) {
    return std::clamp(static_cast<float>(percent), 0.0f, 100.0f) / 100.0f;
}

// Lead times. The call has to land, be understood, and still leave time to act - a play-test found
// 2.5 s arriving about half a second before the corner was already being hit, so it buys the whole
// spoken phrase plus reaction time rather than just the phrase.
constexpr float kCallLeadSec = 4.0f;
constexpr float kApproachLeadSec[] = {2.5f, 1.4f, 0.7f};
constexpr float kApproachPitch[] = {1.0f, 1.25f, 1.5f};
constexpr int kApproachStages = 3;

// A following corner starting this soon after the last one ended has no real straight between
// them, so they are announced together. In stations, like every other distance here.
constexpr float kChainGapStations = 2.0f;
constexpr int kMaxChain = 3;

// A corner stays the one being described until its exit is this far behind, so the cues do not flip
// to the next corner the moment this one's exit is crossed.
constexpr float kClearStations = 1.5f;

constexpr float kBeepHz = 660.0f;  // E5, well clear of the carrier
// Raised from 0.45/0.07 after a play-test could not say for sure the beeps had sounded at all
// against the game's full-volume audio.
constexpr float kBeepAmplitude = 0.6f;
constexpr float kBeepSec = 0.1f;
// Entry, apex and exit rise through the corner, so which of the three you are hearing is obvious
// even when they come close together.
constexpr float kEntryPitch = 1.0f;
constexpr float kApexPitch = 1.25f;
constexpr float kExitPitch = 1.5f;
// Beeps lean towards the OUTSIDE of the corner - the side being drifted into, the one to steer
// away from - so they speak the same direction language as the engine pan and the edge cue:
// sound marks danger. The spoken call already names the turn direction.
constexpr float kBeepPan = 0.8f;

// One key per whole call ("curve_hard_right_long"), so each language file states the exact
// phrase and word order, gender and agreement never leak into code.
std::string CurvePhrase(const Curve& curve) {
    std::string key = "curve_";
    switch (curve.severity) {
        case TurnSeverity::Hairpin:
            key += "hairpin_";
            break;
        case TurnSeverity::Hard:
            key += "hard_";
            break;
        case TurnSeverity::Easy:
            key += "gentle_";
            break;
        case TurnSeverity::Normal:
        default:
            break;
    }
    key += curve.right ? "right" : "left";
    if (curve.isLong) {
        key += "_long";
    }
    return loc::Get(key);
}

CueSpec BeepSpec(float pitch, bool right) {
    CueSpec spec;
    spec.shape = Waveform::Triangle;
    spec.frequencyHz = kBeepHz * pitch;
    spec.amplitude = kBeepAmplitude;
    // A right-hand corner drifts the kart to the LEFT outside, so the beep sounds left.
    spec.pan = right ? -kBeepPan : kBeepPan;
    spec.durationSec = kBeepSec;
    return spec;
}

// Seconds until the kart reaches a point this far ahead, or a large number when stopped.
float LeadSeconds(float distance, float speed) {
    constexpr float kNoLead = 1e9f;
    return speed > 0.0f ? distance / speed : kNoLead;
}

}  // namespace

void DriveAssist::Reset() {
    mSmoothedPan = 0.0f;
    mLastToward = 0.0f;
    mLastLap = -1;
    mActiveEntry = -1;
    mApproachBeeps = 0;
    mAnnounced = false;
    mPhase = 0;
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

    // The aim distance rides on the median corridor half-width, the course's own road scale.
    const float widthScale =
        map.MedianHalfWidth() > 0.0f ? map.MedianHalfWidth() : map.MeanSpacing();
    const float lookAhead = Fraction(RuntimeConfigFile::AccessibilitySteeringLookAhead());
    const float aimWidths = kAimNearWidths + (kAimFarWidths - kAimNearWidths) * lookAhead;

    const float arc = map.ArcOfPosition(state.x, state.z, station);
    float targetX = 0.0f, targetZ = 0.0f;
    float bearing = 0.0f;
    // RouteBased, not Loaded: the checkpoint-midpoint fallback carries progress and corner shape
    // but its midpoints can sit off the road entirely - aiming the engine at them would guide the
    // player into whatever the quads happen to span.
    const bool haveTarget = map.RouteBased() && widthScale > 0.0f &&
                            map.PointAtArc(arc + aimWidths * widthScale, targetX, targetZ) &&
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
    if (std::fabs(bearing) > kAsternRad) {
        toward = mLastToward < 0.0f ? -1.0f : 1.0f;
    } else {
        mLastToward = toward;
    }
    // Negated: `toward` points AT the line, and the player asked for the engine on the side to
    // steer AWAY from ("curva a la izquierda -> motor a la derecha").
    const float pan = -toward * strength;
    mSmoothedPan += (pan - mSmoothedPan) * alpha;
    mLastBearingDeg = bearing / kDegToRad;
    mLastReachWidths = aimWidths;
}

void DriveAssist::UpdateCurveCues(const RaceState& state, const CourseMap& map, int station) {
    const float spacing = map.MeanSpacing();
    if (spacing <= 0.0f) {
        return;
    }

    // Continuous, so the cues fire where the kart actually is. The mapped station alone is
    // checkpoint-coarse - thousands of units on a real course - which put the entry and apex
    // beeps the better part of a second off.
    const float arc = map.ArcOfPosition(state.x, state.z, station);

    const Curve* active = map.ActiveCurveAt(arc, kClearStations * spacing);
    if (active == nullptr) {
        mActiveEntry = -1;
        return;
    }

    if (active->entry != mActiveEntry) {
        mActiveEntry = active->entry;
        mApproachBeeps = 0;
        mAnnounced = false;
        mPhase = 0;
    }

    // Signed, so a corner already under way reads as behind rather than a lap ahead.
    const float toEntry = map.ArcSignedTo(arc, active->entry);
    const float lead = LeadSeconds(toEntry, state.speedPerSecond);

    // A corner that follows the previous one with no real straight between was already announced
    // and counted down as part of that call, so it gets its own traversal beeps and nothing else.
    const bool chained = map.IsChainFollower(*active, kChainGapStations * spacing);

    if (!chained && mApproachBeeps < kApproachStages && toEntry > 0.0f &&
        lead <= kApproachLeadSec[mApproachBeeps]) {
        // Centred, as in MK64: the countdown says *when*, not *where*. The direction is already in
        // the spoken call, and panning it adds a second thing to interpret for no gain.
        CueSpec beep = BeepSpec(kApproachPitch[mApproachBeeps], active->right);
        beep.pan = 0.0f;
        CueService::Instance().PlayOneShot(CueChannel::Curve, beep);
        ++mApproachBeeps;
        // A cue whose failure is silence gets a diagnostic (the play-test could not tell whether
        // these fired at all). Remove once the beeps are confirmed landing by ear.
        RT_LOGF(RT_TAG_A11Y, "curve countdown %d/%d: curve=%d toEntry=%.0f lead=%.1fs\n",
                mApproachBeeps, kApproachStages, active->entry, static_cast<double>(toEntry),
                static_cast<double>(lead));
    }

    if (!mAnnounced && !chained && lead <= kCallLeadSec) {
        mAnnounced = true;
        std::string phrase = CurvePhrase(*active);

        // Walk forward while each next corner starts too soon after the last one ended to count as
        // a straight, so a chicane is heard as one shape instead of two late warnings.
        const Curve* last = active;
        for (int n = 1; n < kMaxChain; ++n) {
            const Curve* following = map.NextCurve(last->exit);
            if (following == nullptr || following->entry == active->entry ||
                map.ArcForward(last->exit, following->entry) > kChainGapStations * spacing) {
                break;
            }
            phrase += loc::Get("curve_then");
            phrase += CurvePhrase(*following);
            last = following;
        }
        // Calls are seconds apart and each one is still useful, so they queue rather than cut.
        ScreenReader::Instance().Speak(phrase, /*interrupt=*/false);
        RT_LOGF(RT_TAG_A11Y, "curve call: \"%s\" curve=%d toEntry=%.0f lead=%.1fs\n",
                phrase.c_str(), active->entry, static_cast<double>(toEntry),
                static_cast<double>(lead));
    }

    // Entry, apex and exit fire on arrival - "have I reached or passed it" - so a landmark skipped
    // in a single frame at racing speed still sounds. One beep per tick: the channel has a single
    // voice and a one-shot restarts it, so two landmarks crossed in the same tick (a two-station
    // corner puts the apex ON the entry) would silently eat each other - the furthest one reached
    // is the truth of where the kart is.
    int reached = mPhase;
    if (reached < 1 && map.ArcSignedTo(arc, active->entry) <= 0.0f) {
        reached = 1;
    }
    if (reached == 1 && map.ArcSignedTo(arc, active->apex) <= 0.0f) {
        reached = 2;
    }
    if (reached >= 1 && reached < 3 && map.ArcSignedTo(arc, active->exit) <= 0.0f) {
        reached = 3;
    }
    if (reached != mPhase) {
        constexpr float kPhasePitch[] = {kEntryPitch, kEntryPitch, kApexPitch, kExitPitch};
        mPhase = reached;
        CueService::Instance().PlayOneShot(CueChannel::Curve,
                                           BeepSpec(kPhasePitch[reached], active->right));
        RT_LOGF(RT_TAG_A11Y, "curve beep phase=%d (1 entry, 2 apex, 3 exit): curve=%d arc=%.0f\n",
                reached, active->entry, static_cast<double>(arc));
    }
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
        mActiveEntry = -1;  // every corner is worth calling again on the next lap
    }

    UpdateSteering(state, map, handedness, station, dtSec);
    UpdateCurveCues(state, map, station);
}

}  // namespace a11y::race
