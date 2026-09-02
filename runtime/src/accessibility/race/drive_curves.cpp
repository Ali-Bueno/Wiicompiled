#include "accessibility/race/drive_assist.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>

#include "accessibility/a11y_log.h"
#include "accessibility/audio/cue_service.h"
#include "accessibility/localization.h"
#include "accessibility/race/anticipation.h"
#include "accessibility/race/course_map.h"
#include "accessibility/race/race_state.h"
#include "accessibility/screen_reader.h"

namespace a11y::race {
namespace {

using audio::CueChannel;
using audio::CueService;
using audio::CueSpec;
using audio::Waveform;

constexpr float kApproachPitch[] = {1.0f, 1.25f, 1.55f};  // MK64 DriveAssist.cpp:80

// A run of corners is one thing to drive (the player's spec, 2026-08-28 and 2026-09-03): the
// phrase names the run, the countdown sounds before its FIRST corner only, every corner keeps
// its entry/apex/exit beeps, and the next run's call waits until the kart is on the straight
// before it. Which corners chain is the course's own (Curve::follower). At most this many
// corners share one phrase.
constexpr int kMaxChain = 3;


// TWO cue families, which must never be mistaken for each other (MK64 AudioCueService.cpp:67-68:
// distinct timbres per family), each on its own voice so neither can eat the other.
constexpr float kBeepAmplitude = 0.6f;

// Approach: smooth sine, higher register, three RISING pitches - it sounds like a countdown.
constexpr Waveform kApproachShape = Waveform::Sine;
constexpr float kApproachHz = 700.0f;   // MK64 AudioCueService.cpp:166
constexpr float kApproachSec = 0.081f;  // MK64's 2600 samples at 32 kHz

// Traversal: hollow square, lower register; entry and apex share a pitch, the exit rises to mark
// the end (MK64 DriveAssist.cpp:90-93).
constexpr Waveform kCurveShape = Waveform::Square;
constexpr float kCurveHz = 440.0f;   // MK64 AudioCueService.cpp:167
constexpr float kCurveSec = 0.094f;  // MK64's 3000 samples at 32 kHz
constexpr float kPhasePitch[] = {1.0f, 1.0f, 1.0f, 1.5f};  // index = phase reached
// Beeps lean towards the OUTSIDE of the corner - the side being drifted into, the one to steer
// away from - the same direction language as the engine pan and the edge cue.
constexpr float kBeepPan = 0.8f;
constexpr float kDemoCurvePan = 0.7f;

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

// Centred, as in MK64: the countdown says *when*, not *where*.
CueSpec ApproachBeep(int stage) {
    CueSpec spec;
    spec.shape = kApproachShape;
    spec.frequencyHz = kApproachHz * kApproachPitch[stage];
    spec.amplitude = kBeepAmplitude;
    spec.pan = 0.0f;
    spec.durationSec = kApproachSec;
    return spec;
}

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

constexpr float kNoLead = 1e9f;

float LeadSeconds(float distance, float speed) {
    return speed > 0.0f ? distance / speed : kNoLead;
}

}  // namespace

void PlayCurveCueDemo() {
    CueSpec beep = LandmarkBeep(kPhasePitch[1], /*right=*/true);
    beep.pan = kDemoCurvePan;
    CueService::Instance().PlayOneShot(CueChannel::Curve, beep);
}

void DriveAssist::RebindCurves(const CourseMap& map) {
    mCurveGeneration = map.CurveGeneration();
    mCues.assign(map.Curves().size(), CornerCues{});
    mPendingLandmarks.clear();
    mPendingRight.clear();
}

// Every corner is scheduled on its own, from its arc position and the kart's time to reach it,
// so the corner being counted down and the corner being driven never compete for one state
// machine. The rules the player set, in order:
//  - the call names the corner (and the corners chained to it) once, never from inside a corner;
//  - a countdown stage that falls inside another corner is forfeited, not deferred;
//  - entry, apex and exit sound on every corner, queued rather than dropped when they coincide.
void DriveAssist::UpdateCurveCues(const RaceState& state, const CourseMap& map, int station,
                                  float dtSec) {
    if (map.CurveGeneration() != mCurveGeneration || mCues.size() != map.Curves().size()) {
        RebindCurves(map);
    }
    const std::vector<Curve>& curves = map.Curves();
    if (curves.empty()) {
        return;
    }

    const float arc = map.ArcOfPosition(state.x, state.z, station);
    const float speed = state.speedPerSecond;
    const float roadWidth = std::max(2.0f * map.MedianHalfWidth(), map.MeanSpacing());
    const float lap = map.LapLength();

    // A respawn moves the kart back further than it could ever drive in a frame - in the WORLD,
    // not just along the arc, which a projection can also do at a crossover: every corner
    // re-arms, so the corner it lands before is announced again.
    if (mLastArc >= 0.0f) {
        const float back = map.ArcBetween(arc, mLastArc);
        const float moved = std::hypot(state.x - mLastX, state.z - mLastZ);
        if (back > roadWidth && back < lap * 0.5f && moved > roadWidth) {
            std::fill(mCues.begin(), mCues.end(), CornerCues{});
            RT_LOGF(RT_TAG_A11Y, "curve cues: re-armed after a %.0f unit jump back\n",
                    static_cast<double>(back));
        }
    }
    mLastArc = arc;
    mLastX = state.x;
    mLastZ = state.z;

    const Curve* inside = map.CurveContaining(arc);

    for (std::size_t i = 0; i < curves.size(); ++i) {
        const Curve& c = curves[i];
        CornerCues& cue = mCues[i];
        const float since = map.ArcBetween(c.entry, arc);  // forward from the entry to the kart
        const float toEntry = map.ArcBetween(arc, c.entry);
        const bool within = since <= c.length;
        const bool justPast = !within && since <= c.length + roadWidth;

        // A corner re-arms once it has been PASSED - more than a road width behind, within the
        // last half lap - and at no other time: what has been said about a corner ahead stays
        // said. (Re-arming by "too far ahead" un-said the second and third corners of a run the
        // moment the phrase named them, so they were named again at the apexes: 2026-09-03.)
        if (!within && since > c.length + roadWidth && since < lap * 0.5f) {
            cue = CornerCues{};
            continue;
        }
        // A corner that is the whole lap (an oval) is never left: back before its apex after
        // passing it is the next lap, and it re-arms there.
        if (within && cue.phase >= 2 && since < c.length * 0.5f) {
            cue = CornerCues{};
        }

        const float lead = LeadSeconds(toEntry, speed);

        // A run's call waits for the straight before it: not merely "outside a corner", which is
        // also true in every gap inside the previous run and had the next run announced while
        // the current one was still being driven (Moo Moo Meadows, 2026-09-03). A follower never
        // opens a phrase; it is named in its leader's, or at its predecessor's apex below.
        const Curve* before = map.CurveBefore(c);
        const bool onStraightBefore =
            before == nullptr || before == &c ||
            map.ArcBetween(map.CurveExit(*before), arc) <= map.GapAfter(*before, c);
        if (!cue.called && !c.follower && !within && inside == nullptr && onStraightBefore &&
            lead <= kSpokenLeadSec) {
            std::string phrase = CurvePhrase(c);
            cue.called = true;
            const Curve* last = &c;
            for (int chained = 1; chained < kMaxChain; ++chained) {
                const Curve* next = map.CurveAfter(*last);
                if (next == nullptr || next == &c) {
                    break;
                }
                const std::size_t k = static_cast<std::size_t>(next - curves.data());
                if (!next->follower || mCues[k].called) {
                    break;
                }
                phrase += loc::Get("curve_then");
                phrase += CurvePhrase(*next);
                mCues[k].called = true;
                last = next;
            }
            // Calls are seconds apart and each one is still useful, so they queue rather than cut.
            ScreenReader::Instance().Speak(phrase, /*interrupt=*/false);
            RT_LOGF(RT_TAG_A11Y, "curve call: \"%s\" curve=%d toEntry=%.0f lead=%.1fs\n",
                    phrase.c_str(), static_cast<int>(i), static_cast<double>(toEntry),
                    static_cast<double>(lead));
        }

        // Only the corner that opens a run counts down; a follower has no straight to count on.
        if (!within && !c.follower && cue.stagesDone < kCountdownStages &&
            lead <= kCountdownLeadSec[cue.stagesDone]) {
            // Only the furthest stage already reached: a respawn landing inside every window
            // must not cram three beeps into three frames.
            int stage = cue.stagesDone;
            while (stage + 1 < kCountdownStages && lead <= kCountdownLeadSec[stage + 1]) {
                ++stage;
            }
            if (inside == nullptr) {
                CueService::Instance().PlayOneShot(CueChannel::Countdown, ApproachBeep(stage));
            }
            RT_LOGF(RT_TAG_A11Y, "curve countdown %d/%d%s: curve=%d toEntry=%.0f lead=%.1fs\n",
                    stage + 1, kCountdownStages, inside == nullptr ? "" : " forfeited",
                    static_cast<int>(i), static_cast<double>(toEntry), static_cast<double>(lead));
            cue.stagesDone = stage + 1;
        }

        if (within || justPast) {
            int reached = cue.phase;
            if (reached < 1 && within) {
                reached = 1;
            }
            if (reached >= 1 && reached < 2 && since >= c.length * 0.5f) {
                reached = 2;
            }
            if (reached >= 1 && reached < 3 && since >= c.length) {
                reached = 3;
            }
            for (int phase = cue.phase + 1; phase <= reached; ++phase) {
                mPendingLandmarks.push_back(kPhasePitch[phase]);
                mPendingRight.push_back(c.right);
                RT_LOGF(RT_TAG_A11Y,
                        "curve beep phase=%d (1 entry, 2 apex, 3 exit): curve=%d arc=%.0f\n",
                        phase, static_cast<int>(i), static_cast<double>(arc));
            }
            // A run longer than one phrase: each corner past the phrase is named at its
            // predecessor's apex - the driver is committed to this corner, the next is what to
            // prepare for - so no corner of a run ever goes unnamed.
            if (cue.phase < 2 && reached >= 2) {
                const Curve* next = map.CurveAfter(c);
                if (next != nullptr && next != &c) {
                    const std::size_t k = static_cast<std::size_t>(next - curves.data());
                    if (next->follower && !mCues[k].called) {
                        mCues[k].called = true;
                        const std::string phrase = CurvePhrase(*next);
                        ScreenReader::Instance().Speak(phrase, /*interrupt=*/false);
                        RT_LOGF(RT_TAG_A11Y, "curve call: \"%s\" curve=%d at the apex of %d\n",
                                phrase.c_str(), static_cast<int>(k), static_cast<int>(i));
                    }
                }
            }
            cue.phase = reached;
        }
    }

    // One landmark voice: coincident landmarks (a short corner's entry and apex in one tick)
    // play one after the other instead of the later one eating the earlier.
    mLandmarkBusySec = std::max(0.0f, mLandmarkBusySec - dtSec);
    if (!mPendingLandmarks.empty() && mLandmarkBusySec <= 0.0f) {
        CueService::Instance().PlayOneShot(
            CueChannel::Curve, LandmarkBeep(mPendingLandmarks.front(), mPendingRight.front()));
        mPendingLandmarks.erase(mPendingLandmarks.begin());
        mPendingRight.erase(mPendingRight.begin());
        mLandmarkBusySec = kCurveSec;
    }
}

}  // namespace a11y::race
