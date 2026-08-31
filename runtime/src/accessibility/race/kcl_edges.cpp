#include <algorithm>
#include <cmath>

#include "accessibility/race/kcl_road.h"

namespace a11y::race {
namespace {

// The march the offline reference table was built with, so a runtime row is diffable against it.
// The step is shared (kcl_road.h); 8000 is past the widest open section, where 4000 truncated
// kinoko's stations 18 and 32 and reported a road that simply had not ended yet.
constexpr float kLateralReachUnits = 8000.0f;
constexpr int kLateralSamples = static_cast<int>(kLateralReachUnits / kKclLateralStepUnits);

// How far the floor may move vertically between two neighbouring lateral samples before it counts
// as a different surface. Follows banking and camber outwards without hopping onto a deck overhead.
constexpr float kEdgeBankWindowUnits = 150.0f;

// The vertical reach of a probe that has no surface to follow yet - the station's own first sample,
// and every look past the edge. It has to clear the tallest thing a route point can be standing
// under: kinoko's station 17 sits 403 units below the mushroom cap that is its real floor, which a
// shorter reach reads as a hole and reports as a fall. It doubles as the definition of a drop: a
// surface further below the road than this is not a shoulder the player can recover from.
constexpr float kEdgeProbeReachUnits = 600.0f;

// How far past the edge to look before concluding there is nothing there. Three march steps clears
// a barrier's own footprint - the offline table settled walls with a 50 unit test radius - without
// reaching so far that the next road over answers for this edge.
constexpr int kEdgeBeyondSteps = 3;

// How far a surface may sit from the road it is being compared with and still be the same road.
// It is the bank window by construction: that IS the definition the sweep already follows sideways
// with, so anything outside it is a different surface. Applied past the edge it settles both traps
// a symmetric window falls into on a tiered course - a deck far below answering "road" and reading
// as a wall, and a bridge overhead answering at all.
constexpr float kEdgeSameSurfaceUnits = kEdgeBankWindowUnits;

// How far the swept surface may drift in height from the station's own before the walk is following
// something else. Without it the sweep can climb a 71 degree slope a step at a time - the bank
// window over one march step - and hand back a parallel deck's edge as this road's. One probe reach
// is the drop already defined as past recovery, so a surface that far from the line is not it.
constexpr float kEdgeDriftLimitUnits = kEdgeProbeReachUnits;

// Steps this side may take: the caller's reach, never past the probe cap, and never fewer than one
// so a station whose stretch is narrower than a step still reports the step it can resolve.
int SamplesFor(float reach) {
    const int steps = static_cast<int>(reach / kKclLateralStepUnits);
    return std::max(1, std::min(steps, kLateralSamples));
}

// Walks outwards one step at a time, carrying the last surface height forward so a sloped road is
// followed rather than dropped. The distance reported is the last sample that was still road, so it
// is a lower bound with the step as its resolution.
KclEdge KclSweepSide(float x, float y, float z, float dirX, float dirZ, int samples) {
    KclEdge edge;
    float referenceY = y;
    float edgeY = y;
    float stopped = 0.0f;
    for (int i = 1; i <= samples; ++i) {
        const float distance = kKclLateralStepUnits * static_cast<float>(i);
        const KclFloor floor = KclRoad::ProbeFloorNear(x + dirX * distance, z + dirZ * distance,
                                                       referenceY, kEdgeBankWindowUnits);
        stopped = distance;
        if (!floor.hit) {
            edge.cause = KclSurface::None;
            break;
        }
        // Climbing away from the line one bank window at a time is how a sweep walks up a wall onto
        // a deck alongside; past the drift limit this is no longer the station's own road.
        if (std::fabs(floor.y - y) > kEdgeDriftLimitUnits) {
            edge.cause = KclSurface::None;
            break;
        }
        if (floor.category != KclSurface::Road) {
            edge.cause = floor.category;
            break;
        }
        edge.distance = distance;
        edgeY = floor.y;
        referenceY = floor.y;
        if (i == samples) {
            // Still road where this station's own stretch ends, so there is no edge to warn about
            // on this side - the same answer the probe cap gives, and for the same reason.
            edge.cause = KclSurface::Road;
            edge.openEnded = true;
            return edge;
        }
    }

    // What is actually out there, from the height of the last road sample. Every step is looked at,
    // never just the first: a 50 unit grass verge with a cliff behind it is a fall, and stopping at
    // the verge would have called it recoverable. Only road behind the boundary is conclusive.
    bool fall = false;
    for (int k = 1; k <= kEdgeBeyondSteps; ++k) {
        const float distance = stopped + kKclLateralStepUnits * static_cast<float>(k);
        const KclFloor floor = KclRoad::ProbeFloorNear(x + dirX * distance, z + dirZ * distance,
                                                       edgeY, kEdgeProbeReachUnits);
        if (!floor.hit || floor.y > edgeY + kEdgeSameSurfaceUnits) {
            continue;  // nothing there, or a bridge overhead: neither is ground beyond this edge
        }
        edge.beyondHit = true;
        if (edgeY - floor.y > kEdgeSameSurfaceUnits) {
            fall = true;  // a deck far below is a drop whatever it is paved with
            continue;
        }
        if (floor.category == KclSurface::Road) {
            edge.beyond = KclSurface::Road;
            break;  // road still behind the boundary: something to scrape along, and conclusive
        }
        if (floor.category == KclSurface::Fall) {
            fall = true;
        } else if (edge.beyond == KclSurface::None) {
            edge.beyond = floor.category;
        }
    }
    if (fall) {
        edge.beyond = KclSurface::Fall;  // outranks a shoulder: the worst of the span is the truth
    }
    return edge;
}

}  // namespace

float KclRoad::ProbeReach() { return kEdgeProbeReachUnits; }

// Searched from the line outwards in both directions at once, nearest first, so a point that sits
// just off the asphalt is pulled to the side it actually left. The vertical window is the probe
// reach, which is already the definition of "further down than a shoulder the player can recover
// from": road further from the line than that is another deck, not this one.
bool KclRoad::FindRoad(float x, float y, float z, float rightX, float rightZ, float limit,
                       float& shiftOut) {
    shiftOut = 0.0f;
    if (!Ready()) {
        return false;
    }
    const float length = std::sqrt(rightX * rightX + rightZ * rightZ);
    if (!(length > 0.0f) || !(limit > 0.0f)) {
        return false;
    }
    const float dirX = rightX / length;
    const float dirZ = rightZ / length;

    if (ProbeFloorNear(x, z, y, kEdgeProbeReachUnits).category == KclSurface::Road) {
        return true;  // already on the asphalt: the authored point stands
    }
    const int steps = static_cast<int>(limit / kKclLateralStepUnits);
    for (int i = 1; i <= steps; ++i) {
        const float distance = kKclLateralStepUnits * static_cast<float>(i);
        for (const float sign : {1.0f, -1.0f}) {
            const float px = x + dirX * distance * sign;
            const float pz = z + dirZ * distance * sign;
            if (ProbeFloorNear(px, pz, y, kEdgeProbeReachUnits).category == KclSurface::Road) {
                shiftOut = distance * sign;
                return true;
            }
        }
    }
    return false;
}

KclEdges KclRoad::ProbeEdges(float x, float y, float z, float rightX, float rightZ,
                             float leftReach, float rightReach) {
    KclEdges out;
    if (!Ready()) {
        return out;
    }
    const float length = std::sqrt(rightX * rightX + rightZ * rightZ);
    if (!(length > 0.0f)) {
        return out;
    }
    const float dirX = rightX / length;
    const float dirZ = rightZ / length;

    out.valid = true;
    // The station's own first probe reaches far enough to find a cap standing over the authored
    // point, then the sweep follows whatever that turned out to be.
    out.centre = ProbeFloorNear(x, z, y, kEdgeProbeReachUnits);
    const float referenceY = out.centre.hit ? out.centre.y : y;
    out.right = KclSweepSide(x, referenceY, z, dirX, dirZ, SamplesFor(rightReach));
    out.left = KclSweepSide(x, referenceY, z, -dirX, -dirZ, SamplesFor(leftReach));
    return out;
}

}  // namespace a11y::race
