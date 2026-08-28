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
constexpr float kApproachPitch[] = {1.0f, 1.25f, 1.55f};  // MK64 DriveAssist.cpp:80
constexpr int kApproachStages = 3;

// Two corners are chained - "no real straight between them" - when the gap cannot fit the
// follower's OWN countdown, so it can never be warned separately and has to ride in the leader's
// phrase. Seconds at the current speed, like every other lead here, because a fixed station count
// reads differently on every route density.
//
// Measured against the LAST beep (0.7 s) this was far too narrow, and a logged race showed the
// hole it leaves: gaps of one route station on that course were 1.15 s, outside the chain, so the
// follower got neither the leader's phrase nor a countdown of its own. Curve 6 ("hairpin right")
// was called 0.7 s before its entry with no beeps, and curve 34 ("hairpin left") 0.2 s before,
// every lap. Widening it to the FIRST beep was tried once and rejected because it muted the
// follower's countdown everywhere; the player has since specified that muting as the behaviour
// they want, so the objection no longer applies.
//
// The player's rule for a run of corners, 2026-08-28: "los cues de cuenta regresiva suenan para la
// primera. pero luego para la segunda y sucessivamente no suena mas hasta acabarse las curvas y
// haber una recta por la cual respirar". The countdown belongs to the run, not to each corner in
// it: it leads the first, and nothing else counts down until a straight wide enough to breathe in
// resets the pattern. Every corner still keeps its own entry/apex/exit beeps.
constexpr int kChainLeadStage = 0;
constexpr int kMaxChain = 3;

// A corner stays the one being described until its exit is this far behind, so the cues do not flip
// to the next corner the moment this one's exit is crossed.
constexpr float kClearStations = 1.5f;

// TWO cue families, which must never be mistaken for each other. They used to be the SAME sound -
// one triangle, one 660 Hz base, the same three rising pitches - with only the pan between them,
// which is nothing against a game that pans its own audio: "los has dejado sonando igual tanto
// para la cuenta atrás cuanto para los anuncios de entrada medio y salida de curva y confunden".
// Rebuilt on MK64's families, whose own comment states the rule: "Each cue family uses a distinct
// timbre so they are easy to tell apart by ear (sine = smooth, square = hollow/buzzy, saw =
// bright/harsh)" (MK64 AudioCueService.cpp:67-68).
//
// Raised from 0.45/0.07 after a play-test could not say for sure the beeps had sounded at all
// against the game's full-volume audio. Deliberately ONE amplitude, not MK64's per-shape trim
// (12000 sine against 8500 square): our SampleWaveform already equalises the four shapes for
// perceived loudness (audio/waveform.h:8-9), so trimming again would push the square family under.
constexpr float kBeepAmplitude = 0.6f;

// Approach: smooth sine, higher register, three RISING pitches - it sounds like a countdown.
constexpr Waveform kApproachShape = Waveform::Sine;
constexpr float kApproachHz = 700.0f;   // MK64 AudioCueService.cpp:166
constexpr float kApproachSec = 0.081f;  // MK64's 2600 samples at 32 kHz

// Traversal: hollow square, lower register, and entry and apex SHARE a pitch so that only the exit
// rises - punctuation of where you ARE, not a count of what is coming. MK64 DriveAssist.cpp:90-93:
// "entry and apex share a pitch, the exit is higher to mark the end".
constexpr Waveform kCurveShape = Waveform::Square;
constexpr float kCurveHz = 440.0f;   // MK64 AudioCueService.cpp:167
constexpr float kCurveSec = 0.094f;  // MK64's 3000 samples at 32 kHz
constexpr float kEntryPitch = 1.0f;
constexpr float kApexPitch = 1.0f;
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

// Centred, as in MK64: the countdown says *when*, not *where*. The direction is already in the
// spoken call, and panning it would add a second thing to interpret for no gain.
CueSpec ApproachBeep(int stage) {
    CueSpec spec;
    spec.shape = kApproachShape;
    spec.frequencyHz = kApproachHz * kApproachPitch[stage];
    spec.amplitude = kBeepAmplitude;
    spec.pan = 0.0f;
    spec.durationSec = kApproachSec;
    return spec;
}

// Panned, unlike MK64, which centres both families. This mod's direction language is "the sound
// marks the side to steer away from", shared with the engine pan and the edge cue - and keeping it
// gives a THIRD axis of separation from the centred countdown, on top of timbre and register.
CueSpec LandmarkBeep(float pitch, bool right) {
    CueSpec spec;
    spec.shape = kCurveShape;
    spec.frequencyHz = kCurveHz * pitch;
    spec.amplitude = kBeepAmplitude;
    // A right-hand corner drifts the kart to the LEFT outside, so the beep sounds left.
    spec.pan = right ? -kBeepPan : kBeepPan;
    spec.durationSec = kCurveSec;
    return spec;
}

// Seconds until the kart reaches a point this far ahead, or a large number when stopped.
float LeadSeconds(float distance, float speed) {
    constexpr float kNoLead = 1e9f;
    return speed > 0.0f ? distance / speed : kNoLead;
}

}  // namespace

void PlayCurveCueDemo() {
    CueSpec beep = LandmarkBeep(kEntryPitch, /*right=*/true);  // entry beep, the representative one
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
        mPhase = 0;
    }

    // The countdown leads the corner AHEAD, never the one being described. ActiveCurveAt picks the
    // smallest SIGNED toEntry, so a corner the kart is inside wins outright over anything coming:
    // the follower of a pair could not begin counting until the leader's exit was behind it, which
    // on a hairpin-then-hard-right is no warning at all - the beeps land with the corner already
    // under way ("suena pero ya no me da tiempo a girar"). The chain veto below still silences a
    // corner too close to be worth its own warning, so the two rules compose instead of one
    // starving the other.
    const Curve* upcoming = map.NextCurve(station);
    if (upcoming == nullptr) {
        upcoming = active;
    }
    if (upcoming->entry != mApproachEntry) {
        mApproachEntry = upcoming->entry;
        mApproachBeeps = 0;
        mAnnounced = false;
    }

    // Signed, so a corner already under way reads as behind rather than a lap ahead.
    const float toEntry = map.ArcSignedTo(arc, upcoming->entry);
    const float lead = LeadSeconds(toEntry, state.speedPerSecond);

    // A corner that follows the previous one with no real straight between was already announced
    // and counted down as part of that call, so it gets its own traversal beeps and nothing else.
    const float chainGap = state.speedPerSecond * kApproachLeadSec[kChainLeadStage];
    const bool chained = map.IsChainFollower(*upcoming, chainGap);
    // Spoken once: as its own call or inside a predecessor's chained phrase. The ledger also
    // vetoes the countdown - the gap is speed-relative, so a corner merged into a phrase at
    // approach speed must not "unchain" and count down just because the kart arrives slower.
    const bool spokenInChain =
        std::find(mChainAnnounced.begin(), mChainAnnounced.end(), upcoming->entry) !=
        mChainAnnounced.end();

    // Whether the kart is still driving the corner being described while a DIFFERENT corner is the
    // one being counted down - the exact condition under which the two cue families overlap.
    const bool insideCorner = active != upcoming && map.ArcSignedTo(arc, active->entry) <= 0.0f &&
                              map.ArcSignedTo(arc, active->exit) > 0.0f;

    // The traversal landmark this tick would beep, resolved BEFORE the countdown so the two cue
    // families cannot collide: the Curve channel is a single voice and a one-shot restarts it, so
    // an approach beep issued in the same tick as an entry/apex/exit beep would silently eat it.
    // The traversal beep wins - it marks where the kart IS - and the approach stage waits a tick.
    //
    // This replaces a rule that consumed the follower's countdown outright whenever a beep would
    // have fallen inside the corner being driven. It was written when both families were the same
    // sound and a countdown really could be mistaken for a traversal beep; they are now a centred
    // sine against a panned square, and the log it was meant to protect shows what it cost -
    // curve 34, a hairpin, reduced to one beep 0.2 s before its entry.
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
    const bool landmarkThisTick = reached != mPhase;

    // Only the corner that OPENS a run counts down. A follower was already named inside the
    // leader's phrase and keeps just its entry/apex/exit beeps, so the run reads as one continuous
    // thing to drive rather than three overlapping warnings.
    const int fromStage = mApproachBeeps;
    const bool countdownDue = !chained && !spokenInChain && !landmarkThisTick &&
                              fromStage < kApproachStages && toEntry > 0.0f &&
                              lead <= kApproachLeadSec[fromStage];
    if (countdownDue) {
        // A crash or respawn can land the kart already inside every countdown window; playing the
        // whole cascade then crams three beeps into three frames (heard live on kinoko's curve
        // 10). Skip to the furthest stage already reached and play only that one - same rule the
        // entry/apex/exit beeps follow.
        int stage = fromStage;
        while (stage + 1 < kApproachStages && lead <= kApproachLeadSec[stage + 1]) {
            ++stage;
        }
        CueService::Instance().PlayOneShot(CueChannel::Curve, ApproachBeep(stage));
        mApproachBeeps = stage + 1;
        // A cue whose failure is silence gets a diagnostic (the play-test could not tell whether
        // these fired at all). Remove once the beeps are confirmed landing by ear.
        RT_LOGF(RT_TAG_A11Y, "curve countdown %d/%d: curve=%d toEntry=%.0f lead=%.1fs\n",
                mApproachBeeps, kApproachStages, upcoming->entry, static_cast<double>(toEntry),
                static_cast<double>(lead));
    }

    // `chained` alone cannot gate the call - a follower past the kMaxChain cap was never spoken,
    // and silencing it would drop a corner entirely (MK64 gates only the countdown on the chain).
    // `toEntry > 0`: the call is anticipation - a corner already under way (re-activated after a
    // crash or spin) must not be announced with a negative lead, which a real lap's log showed.
    // Naming the NEXT corner while the kart is still in this one is what the countdown fix cost:
    // "en el momento en que estoy haciendo una curva me dice otra y aveces confunde". The phrase
    // waits for the exit. MK64 never speaks mid-corner either: a follower is folded into the
    // predecessor's phrase, spoken before the pair.
    if (!mAnnounced && !spokenInChain && !insideCorner && toEntry > 0.0f &&
        lead <= kCallLeadSec) {
        mAnnounced = true;
        std::string phrase = CurvePhrase(*upcoming);

        // Walk forward while each next corner starts too soon after the last one ended to count as
        // a straight, so a chicane is heard as one shape instead of two late warnings.
        const Curve* last = upcoming;
        for (int n = 1; n < kMaxChain; ++n) {
            const Curve* following = map.NextCurve(last->exit);
            if (following == nullptr || following->entry == upcoming->entry ||
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
                phrase.c_str(), upcoming->entry, static_cast<double>(toEntry),
                static_cast<double>(lead));
    }

    // Entry, apex and exit fire on arrival - "have I reached or passed it" - so a landmark skipped
    // in a single frame at racing speed still sounds. One beep per tick: the channel has a single
    // voice and a one-shot restarts it, so two landmarks crossed in the same tick (a two-station
    // corner puts the apex ON the entry) would silently eat each other - the furthest one reached
    // is the truth of where the kart is.
    if (landmarkThisTick) {
        constexpr float kPhasePitch[] = {kEntryPitch, kEntryPitch, kApexPitch, kExitPitch};
        // The pitch of the EARLIEST landmark newly reached, not the furthest. EmitCurve puts the
        // apex at entry + steps/2, so a corner spanning one station has apex == entry and a
        // zero-length one has all three together; taking the furthest played the apex or exit
        // pitch and the ENTRY beep - the one that says turn in now - never sounded at all on the
        // tightest corners of the logged race (curves 6 and 32, both hairpin-grade, and curve 30,
        // a single station). The phase still advances to the furthest, so the exit still hands the
        // focus to the next corner.
        const float pitch = kPhasePitch[mPhase + 1];
        mPhase = reached;
        CueService::Instance().PlayOneShot(CueChannel::Curve, LandmarkBeep(pitch, active->right));
        RT_LOGF(RT_TAG_A11Y, "curve beep phase=%d (1 entry, 2 apex, 3 exit): curve=%d arc=%.0f\n",
                reached, active->entry, static_cast<double>(arc));
    }
    if (mPhase == 3) {
        mFinishedEntry = mActiveEntry;  // done: it must not take the focus back
    }
}

}  // namespace a11y::race
