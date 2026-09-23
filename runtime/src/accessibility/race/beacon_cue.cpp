#include "accessibility/race/beacon_cue.h"

#include "accessibility/race/course_map.h"
#include "accessibility/race/edge_map.h"
#include "accessibility/race/race_state.h"

namespace a11y::race {

float BeaconRoadHalfWidth(const CourseMap& map) {
    const float real = EdgeMap::MedianHalfWidth();
    return real > 0.0f ? real : map.MedianHalfWidth();
}

float RoadAcross(const CourseMap& map, float arc, float x, float z) {
    float px = 0.0f, pz = 0.0f, rightX = 0.0f, rightZ = 0.0f;
    map.PointAtArc(arc, px, pz);
    map.RightVectorAtArc(arc, rightX, rightZ);
    return (x - px) * rightX + (z - pz) * rightZ;
}

float RoadFramePan(const CourseMap& map, float kartArc, const RaceState& state, float targetArc,
                   float targetX, float targetZ, float scale) {
    if (!(scale > 0.0f)) {
        return 0.0f;
    }
    const float across = RoadAcross(map, targetArc, targetX, targetZ) -
                         RoadAcross(map, kartArc, state.x, state.z);
    return std::clamp(across / scale, -1.0f, 1.0f);
}

}  // namespace a11y::race
