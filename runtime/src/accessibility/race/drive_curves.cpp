#include "accessibility/race/drive_assist.h"

#include <algorithm>
#include <string>

#include "accessibility/a11y_log.h"
#include "accessibility/audio/cue_service.h"
#include "accessibility/localization.h"
#include "accessibility/race/course_map.h"
#include "accessibility/race/race_state.h"
#include "accessibility/screen_reader.h"

namespace a11y::race {
namespace {

using audio::CueChannel;
using audio::CueService;
using audio::CueSpec;
using audio::Waveform;

// Lead times. The call has to land, be understood, and still leave time to act - a play-test found
// 2.5 s arriving about half a second before the corner was already being hit, so it buys the whole
// spoken phrase plus reaction time rather than just the phrase.
constexpr float kCallLeadSec = 4.0f;
constexpr float kApproachLeadSec[] = {2.5f, 1.4f, 0.7f};
constexpr float kApproachPitch[] = {1.0f, 1.25f, 1.5f};
constexpr int kApproachStages = 3;

// Two corners are chained - "no real straight between them" - when the gap cannot fit the
// follower's LAST approach beep, so its countdown could not be told apart from the corner the kart
// is still in. Measured against the first beep instead, a chain reached 2.5 s of travel - about ten
// median half-widths, five road widths - and swallowed most corner pairs on the course: the
// follower lost its countdown and was folded into a phrase spoken before its predecessor. Seconds
// at the current speed, like every other lead here, because a fixed station count reads differently
// on every route density.
constexpr int kChainLeadStage = kApproachStages - 1;
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
constexpr float kDemoCurvePan = 0.7f;  // demo: el lado del peligro suena a la derecha

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

void PlayCurveCueDemo() {
    CueSpec beep = BeepSpec(kEntryPitch, /*right=*/true);  // entry beep, the representative one
    beep.pan = kDemoCurvePan;
    CueService::Instance().PlayOneShot(CueChannel::Curve, beep);
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
    // Once a corner's exit beep has sounded, the focus is handed to the next corner at once
    // instead of waiting out the clearance margin. The margin exists to stop the choice
    // flickering BEFORE the exit; holding it after cost the next corner its whole countdown
    // window - a real lap showed curve 10's 2.5 s beep never firing in five sessions because
    // curve 6 kept the focus until 0.8 s out, exactly where the player kept falling.
    if (active != nullptr && active->entry == mFinishedEntry) {
        const Curve* next = map.ActiveCurveAt(arc, 0.0f);
        if (next != nullptr && next->entry != mFinishedEntry) {
            active = next;
        }
    }
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
    const float chainGap = state.speedPerSecond * kApproachLeadSec[kChainLeadStage];
    const bool chained = map.IsChainFollower(*active, chainGap);
    // Spoken once: as its own call or inside a predecessor's chained phrase. The ledger also
    // vetoes the countdown - the gap is speed-relative, so a corner merged into a phrase at
    // approach speed must not "unchain" and count down just because the kart arrives slower.
    const bool spokenInChain =
        std::find(mChainAnnounced.begin(), mChainAnnounced.end(), active->entry) !=
        mChainAnnounced.end();

    if (!chained && !spokenInChain && mApproachBeeps < kApproachStages && toEntry > 0.0f &&
        lead <= kApproachLeadSec[mApproachBeeps]) {
        // A crash or respawn can land the kart already inside every countdown window; playing the
        // whole cascade then crams three beeps into three frames (heard live on kinoko's curve
        // 10). Skip to the furthest stage already reached and play only that one - same rule the
        // entry/apex/exit beeps follow.
        int stage = mApproachBeeps;
        while (stage + 1 < kApproachStages && lead <= kApproachLeadSec[stage + 1]) {
            ++stage;
        }
        // Centred, as in MK64: the countdown says *when*, not *where*. The direction is already in
        // the spoken call, and panning it adds a second thing to interpret for no gain.
        CueSpec beep = BeepSpec(kApproachPitch[stage], active->right);
        beep.pan = 0.0f;
        CueService::Instance().PlayOneShot(CueChannel::Curve, beep);
        mApproachBeeps = stage + 1;
        // A cue whose failure is silence gets a diagnostic (the play-test could not tell whether
        // these fired at all). Remove once the beeps are confirmed landing by ear.
        RT_LOGF(RT_TAG_A11Y, "curve countdown %d/%d: curve=%d toEntry=%.0f lead=%.1fs\n",
                mApproachBeeps, kApproachStages, active->entry, static_cast<double>(toEntry),
                static_cast<double>(lead));
    }

    // `chained` alone cannot gate the call - a follower past the kMaxChain cap was never spoken,
    // and silencing it would drop a corner entirely (MK64 gates only the countdown on the chain).
    // `toEntry > 0`: the call is anticipation - a corner already under way (re-activated after a
    // crash or spin) must not be announced with a negative lead, which a real lap's log showed.
    if (!mAnnounced && !spokenInChain && toEntry > 0.0f && lead <= kCallLeadSec) {
        mAnnounced = true;
        std::string phrase = CurvePhrase(*active);

        // Walk forward while each next corner starts too soon after the last one ended to count as
        // a straight, so a chicane is heard as one shape instead of two late warnings.
        const Curve* last = active;
        for (int n = 1; n < kMaxChain; ++n) {
            const Curve* following = map.NextCurve(last->exit);
            if (following == nullptr || following->entry == active->entry ||
                map.ArcForward(last->exit, following->entry) > chainGap) {
                break;
            }
            phrase += loc::Get("curve_then");
            phrase += CurvePhrase(*following);
            if (std::find(mChainAnnounced.begin(), mChainAnnounced.end(), following->entry) ==
                mChainAnnounced.end()) {
                mChainAnnounced.push_back(following->entry);
            }
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
    if (mPhase == 3) {
        mFinishedEntry = mActiveEntry;  // done: it must not take the focus back
    }
}

}  // namespace a11y::race
