#include "accessibility/race/route_graph.h"

#include <algorithm>
#include <cmath>

#include "accessibility/a11y_log.h"

namespace a11y::race {
namespace {

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
    mLap.clear();
}

const RoutePoint& RouteGraph::Point(int index) const {
    static const RoutePoint kEmpty;
    if (index < 0 || index >= static_cast<int>(mPoints.size())) {
        return kEmpty;
    }
    return mPoints[index];
}

void RouteGraph::Build(std::vector<RoutePoint> points, std::uint8_t startPoint) {
    Clear();
    mPoints = std::move(points);
    BuildLap(startPoint);

    RT_LOGF(RT_TAG_A11Y, "route graph: %d points, lap of %d\n", PointCount(),
            static_cast<int>(mLap.size()));
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
