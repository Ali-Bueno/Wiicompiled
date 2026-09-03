#include "accessibility/race/route_graph.h"

#include <algorithm>
#include <cmath>

#include "accessibility/a11y_log.h"

namespace a11y::race {
namespace {

// How far the smoothing may move a point, as a fraction of that point's own corridor half-width.
// Keeps the smoothed line inside the innermost quarter of the game's own AI corridor, so smoothing
// can de-noise a dense route but can never relocate a coarse one off the road.
constexpr float kSmoothingMaxCorridorFraction = 0.25f;

// Squared distance from a point to a segment, and where along the segment the closest point fell.
float SegmentDistanceSq(float px, float py, float pz, const RoutePoint& a, const RoutePoint& b,
                        float& tOut) {
    const float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
    const float lenSq = ux * ux + uy * uy + uz * uz;
    float t = 0.0f;
    if (lenSq > 0.0f) {
        t = ((px - a.x) * ux + (py - a.y) * uy + (pz - a.z) * uz) / lenSq;
        t = std::clamp(t, 0.0f, 1.0f);
    }
    tOut = t;
    const float dx = px - (a.x + ux * t);
    const float dy = py - (a.y + uy * t);
    const float dz = pz - (a.z + uz * t);
    return dx * dx + dy * dy + dz * dz;
}

}  // namespace

void RouteGraph::Clear() {
    mPoints.clear();
    mAuthored.clear();
    mLap.clear();
}

const RoutePoint& RouteGraph::Point(int index) const {
    static const RoutePoint kEmpty;
    if (index < 0 || index >= static_cast<int>(mPoints.size())) {
        return kEmpty;
    }
    return mPoints[index];
}

const RoutePoint& RouteGraph::Authored(int index) const {
    static const RoutePoint kEmpty;
    if (index < 0 || index >= static_cast<int>(mAuthored.size())) {
        return kEmpty;
    }
    return mAuthored[index];
}

void RouteGraph::Build(std::vector<RoutePoint> points, std::uint8_t startPoint) {
    Clear();
    mPoints = std::move(points);
    mAuthored = mPoints;
    SmoothPositions();
    BuildLap(startPoint);

    RT_LOGF(RT_TAG_A11Y, "route graph: %d points, lap of %d\n", PointCount(),
            static_cast<int>(mLap.size()));
}

// A 3-tap moving average of each point with its neighbours. Measured offline over 4 courses and
// both line sources: this alone beats a KCL-recentred line in 7 of 8 cases, and on Mushroom Gorge's
// mushroom crossing it cuts the line's turn-per-arc to a third (ENPT 1.88->1.21, ITPT 2.72->1.30
// deg per 1000 units) - the authored points carry per-point noise the road does not have.
//
// Over the successor links rather than over entry order, because the route branches: a point's
// neighbours are the points it is joined to. Each side contributes one tap however many links it
// has, so a junction is not dragged towards whichever branch happens to be busier, and a point
// missing a side keeps its own position weighted accordingly instead of being pulled by the taps
// it does not have.
//
// Horizontal only. The authored heights stay exactly as the game states them - that is what lets
// LateralOffset use height to tell the two levels of a crossover apart - and so do the ranges, so
// the corridor half-widths are untouched.
void RouteGraph::SmoothPositions() {
    const int n = PointCount();
    if (n < kMinSmoothPoints) {
        return;
    }

    std::vector<float> beforeX(n, 0.0f), beforeZ(n, 0.0f);
    std::vector<int> beforeCount(n, 0);
    for (int i = 0; i < n; ++i) {
        const RoutePoint& point = mPoints[i];
        for (std::uint8_t k = 0; k < point.nextCount && k < kMaxRouteLinks; ++k) {
            const int j = point.next[k];
            if (j >= n) {
                continue;
            }
            beforeX[j] += point.x;
            beforeZ[j] += point.z;
            ++beforeCount[j];
        }
    }

    std::vector<RoutePoint> smoothed(mPoints);
    for (int i = 0; i < n; ++i) {
        const RoutePoint& point = mPoints[i];
        float sumX = point.x, sumZ = point.z;
        int taps = 1;
        if (beforeCount[i] > 0) {
            sumX += beforeX[i] / static_cast<float>(beforeCount[i]);
            sumZ += beforeZ[i] / static_cast<float>(beforeCount[i]);
            ++taps;
        }
        float afterX = 0.0f, afterZ = 0.0f;
        int afterCount = 0;
        for (std::uint8_t k = 0; k < point.nextCount && k < kMaxRouteLinks; ++k) {
            const int j = point.next[k];
            if (j >= n) {
                continue;
            }
            afterX += mPoints[j].x;
            afterZ += mPoints[j].z;
            ++afterCount;
        }
        if (afterCount > 0) {
            sumX += afterX / static_cast<float>(afterCount);
            sumZ += afterZ / static_cast<float>(afterCount);
            ++taps;
        }

        // Clamped to a fraction of this point's OWN corridor, because an average moves a coarse
        // route much further than a dense one. On Mushroom Gorge the ENPT points average 5028
        // units apart against a 750-unit corridor half-width, and the unclamped average cut the
        // first left-hander by 1300-3000 units - off the asphalt and inside the cliff for some
        // 5100 units, where the raw authored points probe 100% road. Scaled rather than skipped,
        // so the line stays continuous; per point, so a dense stretch still de-noises fully. A
        // point with no stated range cannot say how far is safe, so it does not move at all.
        float dx = sumX / static_cast<float>(taps) - point.x;
        float dz = sumZ / static_cast<float>(taps) - point.z;
        const float maxDisplacement =
            point.range * kCorridorPerRange * kSmoothingMaxCorridorFraction;
        const float displacement = std::sqrt(dx * dx + dz * dz);
        if (displacement > maxDisplacement) {
            const float scale = maxDisplacement / displacement;
            dx *= scale;
            dz *= scale;
        }
        smoothed[i].x = point.x + dx;
        smoothed[i].z = point.z + dz;
    }
    mPoints.swap(smoothed);
}

// Follows the route from the grid's start point until it comes back on itself, and keeps the cycle.
//
// Only the first successor is taken at a branch, so what comes out is one drivable lap rather than
// every path through the course. That is enough for progress and for reading corners; the lateral
// query below still sees every segment, so a player who takes the other branch is measured against
// the road they are actually on.
void RouteGraph::BuildLap(std::uint8_t startPoint) {
    const int n = PointCount();
    if (n <= 0) {
        return;
    }

    std::vector<int> visitOrder(n, -1);
    std::vector<int> path;
    path.reserve(n);

    int current = startPoint < n ? startPoint : 0;
    for (int step = 0; step <= n; ++step) {
        if (visitOrder[current] >= 0) {
            // Came back on itself: the lap is the cycle, so any lead-in before it is dropped.
            mLap.assign(path.begin() + visitOrder[current], path.end());
            return;
        }
        visitOrder[current] = static_cast<int>(path.size());
        path.push_back(current);

        const RoutePoint& point = mPoints[current];
        if (point.nextCount == 0) {
            break;  // a dead-end branch, so this walk is not a lap
        }
        current = point.next[0];
        if (current >= n) {
            break;
        }
    }

    // Never closed. The caller falls back to the checkpoint list rather than driving on a line
    // that does not return to where it started.
    RT_LOGF(RT_TAG_A11Y, "route graph: walk from %u never closed a lap\n",
            static_cast<unsigned>(startPoint));
    mLap.clear();
}

bool RouteGraph::LateralOffset(float x, float y, float z, float rightSign, float& out,
                               float* closestXOut, float* closestZOut,
                               float* halfWidthOut) const {
    out = 0.0f;
    if (mPoints.empty()) {
        return false;
    }

    const RoutePoint* bestA = nullptr;
    const RoutePoint* bestB = nullptr;
    float bestDistSq = 0.0f;
    float bestT = 0.0f;

    // Every edge of the graph, not only the walked lap: on a branching course the kart may be on
    // the path the walk did not take, and it is still road.
    for (const RoutePoint& a : mPoints) {
        for (std::uint8_t k = 0; k < a.nextCount; ++k) {
            const RoutePoint& b = Point(a.next[k]);
            float t = 0.0f;
            const float distSq = SegmentDistanceSq(x, y, z, a, b, t);
            if (bestA == nullptr || distSq < bestDistSq) {
                bestA = &a;
                bestB = &b;
                bestDistSq = distSq;
                bestT = t;
            }
        }
    }
    if (bestA == nullptr) {
        return false;
    }

    // Direction of the segment in the horizontal plane, which is what "across the road" is
    // measured against.
    float fx = bestB->x - bestA->x;
    float fz = bestB->z - bestA->z;
    const float len = std::sqrt(fx * fx + fz * fz);
    if (len <= 0.0f) {
        return false;
    }
    fx /= len;
    fz /= len;

    const float closestX = bestA->x + (bestB->x - bestA->x) * bestT;
    const float closestZ = bestA->z + (bestB->z - bestA->z) * bestT;
    if (closestXOut != nullptr) {
        *closestXOut = closestX;
    }
    if (closestZOut != nullptr) {
        *closestZOut = closestZ;
    }
    const float lateral = (x - closestX) * (fz * rightSign) + (z - closestZ) * (-fx * rightSign);

    const float halfWidth =
        (bestA->range + (bestB->range - bestA->range) * bestT) * kCorridorPerRange;
    if (halfWidth <= 0.0f) {
        return false;
    }
    if (halfWidthOut != nullptr) {
        *halfWidthOut = halfWidth;
    }

    out = lateral / halfWidth;
    return true;
}

}  // namespace a11y::race
