#ifndef MKW_ACCESSIBILITY_RACE_LOWER_LANE_H
#define MKW_ACCESSIBILITY_RACE_LOWER_LANE_H

#include "accessibility/race/edge_map.h"

namespace a11y::race {

class CourseMap;
struct RaceState;

// The kart on the road at the foot of a ledge. The line stays on the road above, so while the kart
// is down there the guide and the edge cues measure the road it is actually on, until the stations
// stop having a road below on that side (RR N64 Luigi Raceway's first curve, docs/driving-review.md).
struct LowerLane {
    static void Reset();
    // Once per frame, before the guide and the edge cues read it. Only a floor under the kart
    // clearly below the edge above puts it down there, never its lateral alone: a kart riding the
    // edge on the road above must keep being guided along that road.
    static void Update(const RaceState& state, const CourseMap& map, int station);
    static bool Active();
    // The lower road's middle at `arc`, signed to the track's right; 0 when not active. Ahead of
    // the last station that has one it keeps that station's value, so the guide never turns back
    // towards the line while the step may still stand between them.
    static float CentreAt(const CourseMap& map, float arc);
    // The room either side of that middle and what ends it: the step face towards the line, which
    // from below is a wall, and the lower road's own end away from it. False when not active.
    static bool SideAtArc(const CourseMap& map, float arc, bool right, float& distance,
                          EdgeKind& kind);
};

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_LOWER_LANE_H
