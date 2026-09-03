#ifndef MKW_ACCESSIBILITY_RACE_ROUTE_GRAPH_H
#define MKW_ACCESSIBILITY_RACE_ROUTE_GRAPH_H

#include <cstdint>
#include <vector>

#include "accessibility/race/kmp_reader.h"

namespace a11y::race {

// The AI route as the graph it actually is, and the two things the mod needs from it: an ordered
// lap, and where the kart sits across the road.
//
// This is the answer to the question the whole race assist turned on. A KMP checkpoint quad is a
// lap-validation volume - the game computes its width once and throws it away - so its midpoint is
// not the middle of the road and can sit off the road entirely. The AI route is the line the CPU
// drivers follow, so it is on the road by construction, and the game states a corridor half-width
// alongside every point. What made it unusable before was reading it in entry order: ENPT points
// are grouped and the groups branch, so walking by index zigzags between them and measured a lap
// twice its real length. Following the successor links the game itself computed walks the route.
class RouteGraph {
public:
    // Takes the route in and smooths it once (SmoothPositions), so the lap stations, the steering
    // target and the lateral offset below all measure against the same line. Two geometries meant
    // the guide aimed at a corner-cut line while the offset was measured from the authored one, and
    // a player who followed the guide read as drifting to the inside of every corner.
    void Build(std::vector<RoutePoint> points, std::uint8_t startPoint);
    void Clear();

    bool Loaded() const { return mLap.size() >= kMinLapPoints; }

    // The lap, as indices into the point list, in the order it is driven.
    const std::vector<int>& Lap() const { return mLap; }

    const RoutePoint& Point(int index) const;
    // The same point exactly as the course author placed it, before the smoothing above moved it.
    // The corner model runs on these: the game's own corner test is a turn angle at the authored
    // vertices, and smoothing is what a turn angle is most sensitive to.
    const RoutePoint& Authored(int index) const;
    int PointCount() const { return static_cast<int>(mPoints.size()); }

    // Where the kart sits across the road: -1 at the left edge of the game's own corridor, 0 on
    // the route line, +1 at the right edge. Past 1 in magnitude means outside the corridor.
    //
    // Measured against the nearest route *segment*, chosen in three dimensions so a course that
    // crosses over itself picks the level the kart is actually on. `rightSign` says which of the
    // two horizontal perpendiculars is right; it is settled once, elsewhere, for the whole mod.
    //
    // False when the route did not read, which is the caller's cue to stay silent rather than to
    // guess. `closestX/Z`, when asked for, are the point on the line the offset was measured to,
    // and `halfWidth` the corridor half-width the offset was normalized by.
    bool LateralOffset(float x, float y, float z, float rightSign, float& out,
                       float* closestXOut = nullptr, float* closestZOut = nullptr,
                       float* halfWidthOut = nullptr) const;

private:
    // Below this a walk did not produce a lap and every query would be meaningless.
    static constexpr std::size_t kMinLapPoints = 4;
    // A 3-tap average needs a point that can have a neighbour on each side.
    static constexpr int kMinSmoothPoints = 3;

    void BuildLap(std::uint8_t startPoint);
    // Applied once, by Build, so every consumer measures against one geometry.
    void SmoothPositions();

    std::vector<RoutePoint> mPoints;
    std::vector<RoutePoint> mAuthored;  // as read, kept from before SmoothPositions
    std::vector<int> mLap;
};

// The game's own drivable corridor is this many times a route point's range value.
inline constexpr float kCorridorPerRange = 50.0f;

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_ROUTE_GRAPH_H
