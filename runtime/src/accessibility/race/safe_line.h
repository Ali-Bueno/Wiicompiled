#pragma once
#include <vector>
#include "accessibility/race/edge_map.h"
namespace a11y::race {
// Returns the number of stations where local safety took priority over the slope limit.
int RepairSafeLine(const CourseMap& map, std::vector<StationEdges>& edges,
                   float kartHalfWidth, float warningDistance);
}
