#include "accessibility/race/lower_lane.h"

#include <cmath>

#include "accessibility/a11y_log.h"
#include "accessibility/race/course_map.h"
#include "accessibility/race/kcl_road.h"
#include "accessibility/race/race_state.h"

namespace a11y::race {
namespace {

bool g_active = false;
bool g_right = false;  // the side of the line the lower road is on
int g_kartStation = 0;

float Middle(const LowerRoad& lower) { return 0.5f * (lower.nearDistance + lower.farDistance); }

// The lower road on one side at a continuous arc, blended between the two stations that bracket
// it as SideAtArc blends the edge; false unless both have one.
bool LowerAtArc(const CourseMap& map, float arc, bool right, LowerRoad& out, int& station) {
    float t = 0.0f;
    const int count = map.StationCount();
    if (count <= 0 || !map.SegmentAtArc(arc, station, t)) {
        return false;
    }
    const LowerRoad a = EdgeMap::At(station).Lower(right);
    const LowerRoad b = EdgeMap::At((station + 1) % count).Lower(right);
    if (!a.valid || !b.valid) {
        return false;
    }
    out = a;
    out.nearDistance += (b.nearDistance - a.nearDistance) * t;
    out.farDistance += (b.farDistance - a.farDistance) * t;
    out.edgeY += (b.edgeY - a.edgeY) * t;
    out.farKind = (a.farKind == EdgeKind::Fall || b.farKind == EdgeKind::Fall)
                      ? EdgeKind::Fall
                      : (t < 0.5f ? a.farKind : b.farKind);
    return true;
}

}  // namespace

void LowerLane::Reset() {
    g_active = false;
    g_right = false;
    g_kartStation = 0;
}

bool LowerLane::Active() { return g_active; }

void LowerLane::Update(const RaceState& state, const CourseMap& map, int station) {
    const bool was = g_active;
    g_active = false;
    LowerRoad lower;
    float lateral = 0.0f;
    float floorY = 0.0f;
    if (state.valid && state.driving && map.Loaded() && EdgeMap::Ready() && KclRoad::Ready()) {
        const float arc = map.ArcOfPosition(state.x, state.z, station);
        float offset = 0.0f, corridor = 0.0f;
        if (map.RoadOffsetAtArc(arc, state.x, state.y, state.z, offset, nullptr, nullptr,
                                &corridor)) {
            lateral = offset * corridor;
            const bool right = lateral > 0.0f;
            int at = 0;
            float edge = 0.0f;
            EdgeKind kind = EdgeKind::Unknown;
            if (LowerAtArc(map, arc, right, lower, at) &&
                EdgeMap::SideAtArc(map, arc, right, edge, kind) && std::fabs(lateral) > edge) {
                const KclFloor floor =
                    KclRoad::ProbeFloorNear(state.x, state.z, state.y, KclRoad::ProbeReach());
                floorY = floor.y;
                if (floor.hit && floor.y < lower.edgeY - KclRoad::SameSurfaceWindow()) {
                    g_active = true;
                    g_right = right;
                    g_kartStation = at;
                }
            }
        }
    }
    // Temporary, with the other cue diagnostics: which road the cues measured, for the next log.
    if (g_active != was) {
        if (g_active) {
            RT_LOGF(RT_TAG_A11Y,
                    "lower lane: on at station %d, %s, lateral %.0f, road below %.0f..%.0f, "
                    "floor %.0f under edge %.0f\n",
                    g_kartStation, g_right ? "right" : "left", static_cast<double>(lateral),
                    static_cast<double>(lower.nearDistance), static_cast<double>(lower.farDistance),
                    static_cast<double>(floorY), static_cast<double>(lower.edgeY));
        } else {
            RT_LOGF(RT_TAG_A11Y, "lower lane: off\n");
        }
    }
}

float LowerLane::CentreAt(const CourseMap& map, float arc) {
    if (!g_active) {
        return 0.0f;
    }
    const float sign = g_right ? 1.0f : -1.0f;
    LowerRoad lower;
    int station = 0;
    if (LowerAtArc(map, arc, g_right, lower, station)) {
        return sign * Middle(lower);
    }
    // Back towards the kart to the nearest station that still has it: the road below goes on
    // ahead as it last was until the kart itself reaches the end of it.
    const int count = map.StationCount();
    if (count <= 0) {
        return 0.0f;
    }
    float t = 0.0f;
    if (!map.SegmentAtArc(arc, station, t)) {
        return 0.0f;
    }
    for (int i = station;; i = (i - 1 + count) % count) {
        const LowerRoad here = EdgeMap::At(i).Lower(g_right);
        if (here.valid) {
            return sign * Middle(here);
        }
        if (i == g_kartStation) {
            return 0.0f;
        }
    }
}

bool LowerLane::SideAtArc(const CourseMap& map, float arc, bool right, float& distance,
                          EdgeKind& kind) {
    LowerRoad lower;
    int station = 0;
    if (!g_active || !LowerAtArc(map, arc, g_right, lower, station)) {
        return false;
    }
    distance = 0.5f * (lower.farDistance - lower.nearDistance);
    kind = right == g_right ? lower.farKind : EdgeKind::Wall;
    return distance > 0.0f;
}

}  // namespace a11y::race
