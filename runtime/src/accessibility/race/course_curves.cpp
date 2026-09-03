#include "accessibility/race/course_map.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include "accessibility/a11y_log.h"
#include "accessibility/race/anticipation.h"

namespace a11y::race {
namespace {

// Corner grades by radius, in world units - one ladder on every course and every vehicle, by
// the player's decision. The rungs are the game's own physics for its reference vehicle
// (Standard Kart M with Mario at 150cc, 79.8 units/frame; Kinoko KartMove::calcRotation, stats
// from kartParam.bin/driverParam.bin): full stick without a drift holds a 6,800-unit radius, a
// drift with its outside-drift bonus about half of that. Half the stick is twice the radius.
constexpr float kRadiusHard = 6800.0f;               // a drift is necessary
constexpr float kRadiusNormal = 2.0f * kRadiusHard;  // half stick
constexpr float kRadiusEnter = 4.0f * kRadiusHard;   // a quarter of the stick: a corner begins

// The game's own "this is a corner": the CPU drifts when the signed turn angle at the ENPT vertex
// it is heading for reaches this (func_8073D284, constant at guest 0x808CB04C).
constexpr float kTurnCornerRad = 0.30f;

// And it only starts that drift once it is this close to the turning vertex (func_8072A37C tests
// the route controller's distance to its target against the constant at guest 0x808C9CEC;
// docs/cpu-corner-logic.md). So it bounds both ends of a corner and merges two same-side bends
// closer than this: the CPU never straightens between them.
constexpr float kDriftApproachUnits = 5000.0f;

// A hard corner that turns at least this much overall is a hairpin (the rally pace-note sense).
constexpr float kHairpinDegrees = 120.0f;

constexpr float kPi = 3.14159265358979f;
constexpr float kRadToDeg = 180.0f / kPi;

// Consecutive authored vertices turning the same way - what the CPU drifts through in one go.
struct VertexRun {
    int first = 0;  // lap vertex index, inclusive
    int last = 0;   // lap vertex index, inclusive; wraps past first
    int sign = 0;
    float total = 0.0f;  // signed sum of the turn angles, radians
    float peak = 0.0f;   // largest |turn| of any vertex in the run
    bool forced = false;
};

}  // namespace

// Corners exactly as the game's CPU drivers see them: a signed turn angle at every AUTHORED route
// vertex, runs of vertices turning the same way, and the drift-approach distance both bounding a
// run and joining two runs the driver would never straighten between. The alternative - grading a
// smoothed curvature on the resampled grid - graded the same bend differently per course, because
// the smoothing window was that course's corridor width (312-1211 units).
void CourseMap::BuildCurves() {
    mCurves.clear();
    ++mCurveGeneration;

    const int nv = static_cast<int>(mLapVertices.size());
    const int stations = StationCount();
    if (nv < kMinStations || static_cast<int>(mVertexArc.size()) != nv ||
        !(mVertexArcStep > 0.0f) || stations < kMinStations) {
        RT_LOGF(RT_TAG_A11Y, "curve map: no authored route, no corners\n");
        return;
    }
    const float lap = mVertexArcStep * static_cast<float>(stations);

    auto wrapIndex = [nv](int k) {
        const int m = k % nv;
        return m < 0 ? m + nv : m;
    };
    auto wrapArc = [lap](float d) {
        d = std::fmod(d, lap);
        return d < 0.0f ? d + lap : d;
    };
    auto vertex = [this](int k) -> const RoutePoint& {
        return mGraph.Authored(mLapVertices[static_cast<std::size_t>(k)]);
    };
    auto arcAt = [this](int k) { return mVertexArc[static_cast<std::size_t>(k)]; };
    // The segment leaving vertex k, in the same arc domain as the landmarks.
    auto segment = [&](int k) { return wrapArc(arcAt(wrapIndex(k + 1)) - arcAt(k)); };

    // The turn angle at each vertex, signed right-positive on the map's own perpendicular so that
    // speech, pan and this all share one convention.
    std::vector<float> turn(static_cast<std::size_t>(nv), 0.0f);
    std::vector<int> sign(static_cast<std::size_t>(nv), 0);
    for (int k = 0; k < nv; ++k) {
        const RoutePoint& prev = vertex(wrapIndex(k - 1));
        const RoutePoint& here = vertex(k);
        const RoutePoint& next = vertex(wrapIndex(k + 1));
        float ax = here.x - prev.x, az = here.z - prev.z;
        float bx = next.x - here.x, bz = next.z - here.z;
        const float aLen = std::sqrt(ax * ax + az * az);
        const float bLen = std::sqrt(bx * bx + bz * bz);
        if (!(aLen > 0.0f) || !(bLen > 0.0f)) {
            continue;
        }
        ax /= aLen;
        az /= aLen;
        bx /= bLen;
        bz /= bLen;
        const float rightX = az * mRightPerpSign;
        const float rightZ = -ax * mRightPerpSign;
        const float angle = std::acos(std::clamp(ax * bx + az * bz, -1.0f, 1.0f));
        const float side = bx * rightX + bz * rightZ;
        turn[static_cast<std::size_t>(k)] = side > 0.0f ? angle : -angle;
        // A vertex needing less than a quarter of the stick is a straight one, whatever it says.
        const float localRadius =
            angle > 0.0f ? 0.5f * (aLen + bLen) / angle : std::numeric_limits<float>::max();
        if (localRadius < kRadiusEnter) {
            sign[static_cast<std::size_t>(k)] = side > 0.0f ? 1 : -1;
        }
    }

    // Walking from the least-turning vertex means a corner over the lap seam is walked whole.
    int origin = 0;
    for (int k = 1; k < nv; ++k) {
        if (std::fabs(turn[static_cast<std::size_t>(k)]) <
            std::fabs(turn[static_cast<std::size_t>(origin)])) {
            origin = k;
        }
    }
    std::vector<VertexRun> runs;
    VertexRun open;
    bool isOpen = false;
    for (int step = 1; step <= nv; ++step) {
        const int k = wrapIndex(origin + step);
        const std::size_t u = static_cast<std::size_t>(k);
        const bool forced = vertex(k).driftSetting == kRouteForceDrift;
        if (isOpen && sign[u] != open.sign && sign[u] != 0) {
            runs.push_back(open);
            isOpen = false;
        }
        if (sign[u] == 0 && !forced) {
            if (isOpen) {
                runs.push_back(open);
                isOpen = false;
            }
            continue;
        }
        if (!isOpen) {
            open = VertexRun{};
            open.first = k;
            open.sign = sign[u];
            isOpen = true;
        }
        open.last = k;
        open.total += turn[u];
        open.peak = std::max(open.peak, std::fabs(turn[u]));
        open.forced = open.forced || forced;
    }
    if (isOpen) {
        runs.push_back(open);
    }

    auto join = [](VertexRun& into, const VertexRun& from) {
        into.last = from.last;
        into.total += from.total;
        into.peak = std::max(into.peak, from.peak);
        into.forced = into.forced || from.forced;
    };
    std::vector<VertexRun> merged;
    for (const VertexRun& r : runs) {
        if (!merged.empty()) {
            VertexRun& prev = merged.back();
            if (prev.sign == r.sign &&
                wrapArc(arcAt(r.first) - arcAt(prev.last)) < kDriftApproachUnits) {
                join(prev, r);
                continue;
            }
        }
        merged.push_back(r);
    }
    // The seam again: the scan started at the least-turning vertex, which can still be inside a
    // drift the CPU never released.
    if (merged.size() >= 2 && merged.front().sign == merged.back().sign &&
        wrapArc(arcAt(merged.front().first) - arcAt(merged.back().last)) < kDriftApproachUnits) {
        const VertexRun head = merged.front();
        merged.erase(merged.begin());
        join(merged.back(), head);
    }

    for (const VertexRun& r : merged) {
        const float total = std::fabs(r.total);
        if (total < kTurnCornerRad && !r.forced) {
            continue;
        }
        // The bend reaches back into the segment before its first vertex and on into the one after
        // its last, as far as the CPU's own drift approach and no further.
        const float entryArc = wrapArc(
            arcAt(r.first) - std::min(0.5f * segment(wrapIndex(r.first - 1)), kDriftApproachUnits));
        const float exitArc =
            wrapArc(arcAt(r.last) + std::min(0.5f * segment(r.last), kDriftApproachUnits));
        float length = wrapArc(exitArc - entryArc);
        if (!(length > 0.0f)) {
            length = lap;  // a run that is the whole lap: an oval
        }

        // The corridor absorbs part of a bend: a turn of angle theta made inside a band of
        // half-width w can be driven at a radius up to w*cos(theta/2)/(1-cos(theta/2)), one
        // half-width being the lateral room the racing line actually uses. The band is the game's
        // own CPU corridor, taken at the run's median vertex so one wide point cannot flatten it.
        std::vector<float> widths;
        for (int k = r.first, guard = 0; guard <= nv; ++guard) {
            widths.push_back(vertex(k).range * kCorridorPerRange);
            if (k == r.last) {
                break;
            }
            k = wrapIndex(k + 1);
        }
        std::sort(widths.begin(), widths.end());
        const float halfWidth = widths[widths.size() / 2];
        const float cosHalf = std::cos(std::min(total, kPi) * 0.5f);
        const float absorbed = cosHalf < 1.0f ? halfWidth * cosHalf / (1.0f - cosHalf)
                                              : std::numeric_limits<float>::max();
        const float radius = std::max(length / total, absorbed);
        if (radius >= kRadiusEnter && !r.forced) {
            continue;
        }

        // The apex is where half of the run's turning has been done.
        float turned = 0.0f;
        int apexVertex = r.first;
        for (int k = r.first, guard = 0; guard <= nv; ++guard) {
            turned += std::fabs(turn[static_cast<std::size_t>(k)]);
            if (turned >= total * 0.5f || k == r.last) {
                apexVertex = k;
                break;
            }
            k = wrapIndex(k + 1);
        }

        const float degrees = total * kRadToDeg;
        Curve curve;
        curve.firstVertex = r.first;
        curve.lastVertex = r.last;
        curve.entryPos = entryArc / mVertexArcStep;
        curve.apexPos = arcAt(apexVertex) / mVertexArcStep;
        curve.exitPos = exitArc / mVertexArcStep;
        curve.right = r.sign > 0;
        if (radius < kRadiusHard) {
            curve.severity =
                degrees >= kHairpinDegrees ? TurnSeverity::Hairpin : TurnSeverity::Hard;
        } else if (radius < kRadiusNormal) {
            curve.severity = TurnSeverity::Normal;
        }
        curve.totalDegrees = degrees;
        curve.radius = radius;
        curve.driftPoint = r.peak >= kTurnCornerRad || r.forced;
        curve.forced = r.forced;
        mCurves.push_back(curve);
    }
    std::sort(mCurves.begin(), mCurves.end(),
              [](const Curve& a, const Curve& b) { return a.entryPos < b.entryPos; });

    RefreshCurveArcs();
    LogCurveMap();
}

}  // namespace a11y::race
