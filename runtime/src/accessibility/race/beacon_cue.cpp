#include "accessibility/race/beacon_cue.h"

#include "accessibility/race/course_map.h"
#include "accessibility/race/edge_map.h"
#include "accessibility/race/race_state.h"

namespace a11y::race {

float BeaconRoadHalfWidth(const CourseMap& map) {
    const float real = EdgeMap::MedianHalfWidth();
    return real > 0.0f ? real : map.MedianHalfWidth();
}

float RoadFramePan(const CourseMap& map, float arc, const RaceState& state, float targetX,
                   float targetZ) {
    const float halfWidth = BeaconRoadHalfWidth(map);
    if (!(halfWidth > 0.0f)) {
        return 0.0f;
    }
    float rightX = 0.0f, rightZ = 0.0f;
    map.RightVectorAtArc(arc, rightX, rightZ);
    const float across = (targetX - state.x) * rightX + (targetZ - state.z) * rightZ;
    return std::clamp(across / halfWidth, -1.0f, 1.0f);
}

}  // namespace a11y::race
