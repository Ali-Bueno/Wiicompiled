#include "accessibility/race/course_map.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "accessibility/a11y_log.h"
#include "accessibility/race/anticipation.h"

namespace a11y::race {
namespace {

// Corner grades by radius, in world units - one ladder on every course and every vehicle, by
// the player's decision. The rungs are the game's own physics for its reference vehicle
// (Standard Kart M with Mario at 150cc, 79.8 units/frame; Kinoko KartMove::calcRotation, stats
// from kartParam.bin/driverParam.bin): full stick without a drift holds a 6,800-unit radius, a
// drift with its outside-drift bonus about 3,300. Half the stick is twice the radius, and so on.
constexpr float kRadiusHairpin = 3300.0f;  // even a drift cannot hold it: slow down
constexpr float kRadiusHard = 6800.0f;     // a drift is necessary
constexpr float kRadiusNormal = 2.0f * kRadiusHard;  // half stick
constexpr float kRadiusEnter = 4.0f * kRadiusHard;   // a quarter of the stick: a corner begins
constexpr float kRadiusKeep = 2.0f * kRadiusEnter;   // hysteresis: it lasts while it needs any

// Two same-side bends closer than the last countdown lead at the reference speed are one corner.
constexpr float kMergeGapUnits = kReferenceSpeedUnitsPerSec * kCountdownLeadSec[kCountdownStages - 1];

// A run has to turn at least this much in total to be worth calling, or every wiggle in the line
// becomes a corner. Degrees of heading change, which is spacing-invariant.
constexpr float kCurveMinDegrees = 15.0f;

// A hard corner that turns at least this much overall is a hairpin (the rally pace-note sense).
constexpr float kHairpinDegrees = 120.0f;

constexpr float kPi = 3.14159265358979f;
constexpr float kRadToDeg = 180.0f / kPi;

struct Run {
    int first = 0;  // station index, inclusive
    int last = 0;   // station index, inclusive; wraps past first
    int sign = 0;
    float peak = 0.0f;
    bool entered = false;  // reached the enter threshold, not just the keep one
};

TurnSeverity SeverityFor(float curvature, float totalDegrees) {
    const bool hard = curvature >= 1.0f / kRadiusHard;
    if (curvature >= 1.0f / kRadiusHairpin || (hard && totalDegrees >= kHairpinDegrees)) {
        return TurnSeverity::Hairpin;
    }
    if (hard) {
        return TurnSeverity::Hard;
    }
    if (curvature >= 1.0f / kRadiusNormal) {
        return TurnSeverity::Normal;
    }
    return TurnSeverity::Easy;
}

}  // namespace

// Corners as runs of same-signed curvature on the uniform grid, with hysteresis: a run opens at
// the keep threshold, counts only if it reaches the enter threshold, and closes when the
// curvature drops under keep or changes sign - a chicane is two corners, never one blurred one.
// Same-signed runs too close to count down separately are one corner with a kink in it.
void CourseMap::BuildCurves() {
    const int n = StationCount();
    mCurves.clear();
    ++mCurveGeneration;
    if (n < kMinStations || !(mMeanSpacing > 0.0f)) {
        return;
    }
    const float kappaEnter = 1.0f / kRadiusEnter;
    const float kappaKeep = 1.0f / kRadiusKeep;
    auto curvature = [&](int i) { return mCurvature[static_cast<std::size_t>(Wrap(i))]; };
    auto signOf = [&](float k) { return std::fabs(k) < kappaKeep ? 0 : (k > 0.0f ? 1 : -1); };

    // Scanning starts on a straight, so a corner straddling the finish line is walked whole. A
    // lap with no straight at all is one continuous curve (an oval), emitted as one.
    int origin = -1;
    for (int i = 0; i < n; ++i) {
        if (signOf(curvature(i)) == 0) {
            origin = i;
            break;
        }
    }
    std::vector<Run> runs;
    if (origin < 0) {
        Run whole;
        whole.first = 0;
        whole.last = n - 1;
        float sum = 0.0f;
        for (int i = 0; i < n; ++i) {
            sum += curvature(i);
            whole.peak = std::max(whole.peak, std::fabs(curvature(i)));
        }
        whole.sign = sum >= 0.0f ? 1 : -1;
        whole.entered = true;
        runs.push_back(whole);
    } else {
        Run run;
        bool open = false;
        for (int step = origin + 1; step <= origin + n; ++step) {
            const int i = Wrap(step);
            const float k = curvature(i);
            const int sign = signOf(k);
            if (open && sign != run.sign) {
                if (run.entered) {
                    runs.push_back(run);
                }
                open = false;
            }
            if (sign == 0) {
                continue;
            }
            if (!open) {
                run = Run{};
                run.first = i;
                run.sign = sign;
                open = true;
            }
            run.last = i;
            run.peak = std::max(run.peak, std::fabs(k));
            run.entered = run.entered || std::fabs(k) >= kappaEnter;
        }
        if (open && run.entered) {
            runs.push_back(run);
        }
    }

    // Merge same-signed neighbours whose gap is too short to count down separately - less than
    // the last countdown lead at the reference speed - or shorter than the road is wide. The
    // driver never straightens between them, so they are one corner with a kink.
    const float mergeGapUnits = std::max(2.0f * mMedianHalfWidth, kMergeGapUnits);
    std::vector<Run> merged;
    for (const Run& r : runs) {
        if (!merged.empty()) {
            Run& prev = merged.back();
            const float gap = WrapForward(ArcAt(r.first) - ArcAt(prev.last));
            if (prev.sign == r.sign && gap <= mergeGapUnits) {
                prev.last = r.last;
                prev.peak = std::max(prev.peak, r.peak);
                continue;
            }
        }
        merged.push_back(r);
    }
    // The seam: the scan began on a straight, but that straight can be shorter than the merge gap.
    if (merged.size() >= 2) {
        Run& last = merged.back();
        const Run& first = merged.front();
        if (last.sign == first.sign &&
            WrapForward(ArcAt(first.first) - ArcAt(last.last)) <= mergeGapUnits) {
            last.last = first.last;
            last.peak = std::max(last.peak, first.peak);
            merged.erase(merged.begin());
        }
    }

    for (const Run& r : merged) {
        // Total heading change from the raw per-station turns, shortest way each, so a 230
        // degree hairpin stays 230 degrees.
        float total = 0.0f;
        int count = 0;
        for (int step = r.first;; step = Wrap(step + 1)) {
            total += mRawTurn[static_cast<std::size_t>(step)];
            if (++count > n || step == r.last) {
                break;
            }
        }
        const float totalDegrees = std::fabs(total) * kRadToDeg;
        if (totalDegrees < kCurveMinDegrees) {
            continue;
        }
        Curve curve;
        curve.first = r.first;
        curve.last = r.last;
        // SignedTurnAt already signs against the settled right vector, so this IS the mod's one
        // "right" convention; multiplying by the perpendicular sign again mirrored every corner
        // on the courses that vote -1 (Luigi Circuit, 2026-09-02).
        curve.right = total > 0.0f;
        curve.peakCurvature = r.peak;
        curve.severity = SeverityFor(r.peak, totalDegrees);
        curve.totalDegrees = totalDegrees;
        mCurves.push_back(curve);
    }
    RefreshCurveArcs();
    LogCurveMap();
}

void CourseMap::RefreshCurveArcs() {
    for (Curve& curve : mCurves) {
        // A station's turn happens at the station; the bend occupies half of each neighbouring
        // segment. Real arcs, so the checkpoint fallback's uneven stations are honoured too.
        curve.entry = WrapForward(ArcAt(curve.first) - ArcForward(curve.first - 1, curve.first) * 0.5f);
        const float exit =
            WrapForward(ArcAt(curve.last) + ArcForward(curve.last, curve.last + 1) * 0.5f);
        curve.length = std::max(WrapForward(exit - curve.entry), mMeanSpacing);
        // Long when the corner is at least a straight long: it outlasts its own countdown at
        // the reference speed, so the call says so.
        curve.isLong = curve.length >= kStraightUnits;
    }
    // Runs: a corner closer than a straight to the one before it rides in that one's run. A lap
    // of nothing but corners still needs one leader, the corner after the widest gap.
    const std::size_t count = mCurves.size();
    std::size_t leader = 0;
    float widestGap = -1.0f;
    for (std::size_t i = 0; i < count; ++i) {
        const Curve& prev = mCurves[(i + count - 1) % count];
        const float gap = count > 1 ? GapAfter(prev, mCurves[i]) : mLapLength;
        mCurves[i].follower = gap < kStraightUnits;
        if (gap > widestGap) {
            widestGap = gap;
            leader = i;
        }
    }
    if (count > 0) {
        mCurves[leader].follower = false;
    }
}

// Temporary. Dumps what the segmentation actually produced, so a cue that lands in the wrong place
// can be traced to the map rather than to the cue.
void CourseMap::LogCurveMap() const {
    RT_LOGF(RT_TAG_A11Y, "curve map: %d stations, lap %.0f, spacing %.0f, %d curves\n",
            StationCount(), static_cast<double>(mLapLength), static_cast<double>(mMeanSpacing),
            static_cast<int>(mCurves.size()));
    for (std::size_t i = 0; i < mCurves.size(); ++i) {
        const Curve& c = mCurves[i];
        RT_LOGF(RT_TAG_A11Y,
                "  curve %d entry=%.0f length=%.0f %s severity=%d radius=%.0f total=%.0fdeg "
                "long=%d %s\n",
                static_cast<int>(i), static_cast<double>(c.entry), static_cast<double>(c.length),
                c.right ? "right" : "left", static_cast<int>(c.severity),
                static_cast<double>(c.peakCurvature > 0.0f ? 1.0f / c.peakCurvature : 0.0f),
                static_cast<double>(c.totalDegrees), c.isLong ? 1 : 0,
                c.follower ? "follower" : "leader");
    }
}

const Curve* CourseMap::CurveContaining(float arc) const {
    for (const Curve& curve : mCurves) {
        if (ArcBetween(curve.entry, arc) <= curve.length) {
            return &curve;
        }
    }
    return nullptr;
}

const Curve* CourseMap::CurveAfter(const Curve& from) const {
    const Curve* best = nullptr;
    float bestAhead = 0.0f;
    const float exit = CurveExit(from);
    for (const Curve& curve : mCurves) {
        if (&curve == &from) {
            continue;
        }
        const float ahead = ArcBetween(exit, curve.entry);
        if (best == nullptr || ahead < bestAhead) {
            best = &curve;
            bestAhead = ahead;
        }
    }
    return best != nullptr ? best : (mCurves.empty() ? nullptr : &from);
}

const Curve* CourseMap::CurveBefore(const Curve& from) const {
    const Curve* best = nullptr;
    float bestBehind = 0.0f;
    for (const Curve& curve : mCurves) {
        if (&curve == &from) {
            continue;
        }
        const float behind = ArcBetween(CurveExit(curve), from.entry);
        if (best == nullptr || behind < bestBehind) {
            best = &curve;
            bestBehind = behind;
        }
    }
    return best != nullptr ? best : (mCurves.empty() ? nullptr : &from);
}

float CourseMap::GapAfter(const Curve& from, const Curve& next) const {
    return ArcBetween(CurveExit(from), next.entry);
}

}  // namespace a11y::race
