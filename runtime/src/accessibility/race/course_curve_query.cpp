#include "accessibility/race/course_map.h"

#include <cmath>
#include <cstddef>

#include "accessibility/a11y_log.h"
#include "accessibility/race/anticipation.h"

namespace a11y::race {
namespace {

// One world unit, which is below the precision any arc here is stated to: a "gap" that close to
// the whole lap is two corners touching, measured the wrong way round the seam.
constexpr float kSeamEpsilonUnits = 1.0f;

}  // namespace

void CourseMap::RefreshCurveArcs() {
    // A landmark is a station plus a fraction of the segment leaving it, so it stays the same
    // place on the road however far the racing line has moved the stations sideways.
    auto arcAtPos = [this](float pos) {
        const int station = static_cast<int>(std::floor(pos));
        const float t = pos - static_cast<float>(station);
        return WrapForward(ArcAt(station) + t * ArcForward(station, station + 1));
    };
    for (Curve& curve : mCurves) {
        curve.entry = arcAtPos(curve.entryPos);
        curve.apex = arcAtPos(curve.apexPos);
        curve.length = WrapForward(arcAtPos(curve.exitPos) - curve.entry);
        if (!(curve.length > 0.0f)) {
            curve.length = mLapLength;
        }
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
        float gap = count > 1 ? GapAfter(prev, mCurves[i]) : mLapLength;
        if (gap > mLapLength - kSeamEpsilonUnits) {
            gap = 0.0f;  // touching corners, not a lap-long straight
        }
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
                "  curve %d entry=%.0f apex=%.0f length=%.0f %s severity=%d radius=%.0f "
                "total=%.0fdeg long=%d %s vertices=%d..%d%s%s\n",
                static_cast<int>(i), static_cast<double>(c.entry), static_cast<double>(c.apex),
                static_cast<double>(c.length), c.right ? "right" : "left",
                static_cast<int>(c.severity), static_cast<double>(c.radius),
                static_cast<double>(c.totalDegrees), c.isLong ? 1 : 0,
                c.follower ? "follower" : "leader", c.firstVertex, c.lastVertex,
                c.driftPoint ? " drift" : "", c.forced ? " forced" : "");
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
