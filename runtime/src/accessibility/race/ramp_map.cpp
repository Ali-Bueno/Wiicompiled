#include "accessibility/race/ramp_map.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>

#include "accessibility/a11y_log.h"
#include "accessibility/race/beacon_cue.h"
#include "accessibility/race/course_map.h"
#include "accessibility/race/edge_map.h"
#include "accessibility/race/kcl_launch.h"
#include "accessibility/race/kcl_mesh.h"
#include "accessibility/race/kcl_objects.h"
#include "accessibility/race/kcl_road.h"

namespace a11y::race {
namespace {

// Prisms screened per tick, one attribute read each: the largest course is a few dozen frames.
constexpr std::uint32_t kPrismsPerTick = 4096;
// How far a launch face may sit above or below the line and still be on this stretch of road
// rather than on a deck over or under it. Calibrated on the road corpus (docs/trick-ramp-cue.md).
constexpr float kRampHeightWindow = 1500.0f;
// Faces this close along and across the line are one ramp: the lateral step every distance across
// the road is measured to.
constexpr float kRampJoinUnits = kKclLateralStepUnits;

// Stations whose lateral reach is measured per tick (each is a pass over every station), and faces
// placed per tick (each is a pass over every segment): a long course's placement is a second of
// background work instead of a 100 ms frame (road corpus, 2026-09-23).
constexpr int kReachStationsPerTick = 32;
constexpr std::size_t kPlaceFacesPerTick = 32;

enum class RampMapState { Idle, Scanning, Reaching, Placing, Done };

RampMapState g_state = RampMapState::Idle;
std::uint32_t g_cursor = 1;
std::vector<KclLaunchFace> g_faces;
std::vector<Ramp> g_ramps;
// A rebind keeps answering with the previous ramps until the new ones are placed.
bool g_haveRamps = false;
// Whether the ramps were placed against the shifted line; a change re-places them.
bool g_boundShifted = false;
std::vector<float> g_reachLeft, g_reachRight;
int g_reachCursor = 0;
std::size_t g_faceCursor = 0;

// A face's extent in the line's frame: arcs unwrapped around its own centre, laterals in units.
struct PlacedFace {
    float arcMin = 0.0f, arcMax = 0.0f, latMin = 0.0f, latMax = 0.0f;
};

std::vector<PlacedFace> g_placed;

// The station whose segment owns a point: the nearest one that is level with it and within that
// station's own stretch of course (CourseMap::LateralReach), or -1.
int OwningStation(const CourseMap& map, const std::vector<float>& reachLeft,
                  const std::vector<float>& reachRight, float x, float z, float minY, float maxY) {
    int best = -1;
    float bestScore = std::numeric_limits<float>::max();
    for (int i = 0; i < map.StationCount(); ++i) {
        float ax = 0.0f, az = 0.0f, bx = 0.0f, bz = 0.0f;
        map.Centre(i, ax, az);
        map.Centre(i + 1, bx, bz);
        const float ux = bx - ax, uz = bz - az, lenSq = ux * ux + uz * uz;
        if (lenSq <= 0.0f) {
            continue;
        }
        const float t = std::clamp(((x - ax) * ux + (z - az) * uz) / lenSq, 0.0f, 1.0f);
        const float dx = x - (ax + ux * t), dz = z - (az + uz * t);
        const float dist = std::hypot(dx, dz);
        float rx = 0.0f, rz = 0.0f;
        map.RightVector(i, rx, rz);
        if (dist > (dx * rx + dz * rz > 0.0f ? reachRight[i] : reachLeft[i])) {
            continue;
        }
        const float lineY = map.Height(i) + (map.Height(i + 1) - map.Height(i)) * t;
        const float below = lineY - maxY, above = minY - lineY;
        if (below > kRampHeightWindow || above > kRampHeightWindow) {
            continue;
        }
        const float score = dist + std::max({below, above, 0.0f});
        if (score < bestScore) {
            bestScore = score;
            best = i;
        }
    }
    return best;
}

bool PlaceFace(const CourseMap& map, const std::vector<float>& reachLeft,
               const std::vector<float>& reachRight, const KclLaunchFace& face, PlacedFace& out) {
    const float cx = (face.x[0] + face.x[1] + face.x[2]) / 3.0f;
    const float cz = (face.z[0] + face.z[1] + face.z[2]) / 3.0f;
    const float minY = std::min({face.y[0], face.y[1], face.y[2]});
    const float maxY = std::max({face.y[0], face.y[1], face.y[2]});
    const int owner = OwningStation(map, reachLeft, reachRight, cx, cz, minY, maxY);
    if (owner < 0) {
        return false;
    }
    const float lap = map.LapLength();
    const float centreArc = map.ArcOfPosition(cx, cz, owner);
    out.arcMin = out.latMin = std::numeric_limits<float>::max();
    out.arcMax = out.latMax = std::numeric_limits<float>::lowest();
    for (int k = 0; k < 3; ++k) {
        const float arc = map.ArcOfPosition(face.x[k], face.z[k], owner);
        float along = arc - centreArc;
        if (along > 0.5f * lap) along -= lap;
        if (along < -0.5f * lap) along += lap;
        const float lateral = RoadAcross(map, arc, face.x[k], face.z[k]);
        out.arcMin = std::min(out.arcMin, centreArc + along);
        out.arcMax = std::max(out.arcMax, centreArc + along);
        out.latMin = std::min(out.latMin, lateral);
        out.latMax = std::max(out.latMax, lateral);
    }
    return true;
}

// Whether the ramp touches the road the edge sweep measured, anywhere along it: a launch surface
// out on the grass or past a wall is not something the line's road leads onto. A side the sweep
// could not read counts as open - jump lips are exactly where the line has no floor under it.
bool OnMeasuredRoad(const CourseMap& map, const PlacedFace& ramp) {
    const float step = std::max(map.MeanSpacing(), kRampJoinUnits);
    for (float arc = ramp.arcMin;; arc += step) {
        const float at = map.WrapForward(std::min(arc, ramp.arcMax));
        float left = 0.0f, right = 0.0f;
        EdgeKind kind = EdgeKind::Unknown;
        if (!EdgeMap::SideAtArc(map, at, false, left, kind)) {
            left = std::numeric_limits<float>::max();
        }
        if (!EdgeMap::SideAtArc(map, at, true, right, kind)) {
            right = std::numeric_limits<float>::max();
        }
        if (ramp.latMax >= -left - kRampJoinUnits && ramp.latMin <= right + kRampJoinUnits) {
            return true;
        }
        if (arc >= ramp.arcMax) {
            return false;
        }
    }
}

int Root(std::vector<int>& parent, int i) {
    while (parent[i] != i) {
        parent[i] = parent[parent[i]];
        i = parent[i];
    }
    return i;
}

void StartBind(const CourseMap& map) {
    g_boundShifted = map.RoadShifted();
    g_reachLeft.assign(static_cast<std::size_t>(map.StationCount()), 0.0f);
    g_reachRight.assign(static_cast<std::size_t>(map.StationCount()), 0.0f);
    g_reachCursor = 0;
    g_placed.clear();
    g_faceCursor = 0;
    g_state = RampMapState::Reaching;
}

// Joins touching faces into ramps, once every face is placed.
void Finish(const CourseMap& map) {
    const std::vector<PlacedFace>& placed = g_placed;
    g_ramps.clear();
    std::vector<int> order(placed.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(),
              [&](int a, int b) { return placed[a].arcMin < placed[b].arcMin; });
    std::vector<int> parent(placed.size());
    std::iota(parent.begin(), parent.end(), 0);
    for (std::size_t a = 0; a < order.size(); ++a) {
        const PlacedFace& pa = placed[order[a]];
        for (std::size_t b = a + 1; b < order.size(); ++b) {
            const PlacedFace& pb = placed[order[b]];
            if (pb.arcMin > pa.arcMax + kRampJoinUnits) {
                break;
            }
            if (pb.latMin <= pa.latMax + kRampJoinUnits && pa.latMin <= pb.latMax + kRampJoinUnits) {
                parent[Root(parent, order[a])] = Root(parent, order[b]);
            }
        }
    }
    std::vector<int> slot(placed.size(), -1);
    std::vector<PlacedFace> merged;
    for (std::size_t i = 0; i < placed.size(); ++i) {
        const int root = Root(parent, static_cast<int>(i));
        if (slot[root] < 0) {
            slot[root] = static_cast<int>(merged.size());
            merged.push_back(placed[i]);
            continue;
        }
        PlacedFace& m = merged[slot[root]];
        m.arcMin = std::min(m.arcMin, placed[i].arcMin);
        m.arcMax = std::max(m.arcMax, placed[i].arcMax);
        m.latMin = std::min(m.latMin, placed[i].latMin);
        m.latMax = std::max(m.latMax, placed[i].latMax);
    }
    for (const PlacedFace& m : merged) {
        if (!OnMeasuredRoad(map, m)) {
            continue;
        }
        Ramp ramp;
        ramp.arcStart = map.WrapForward(m.arcMin);
        ramp.length = m.arcMax - m.arcMin;
        ramp.halfWidth = 0.5f * (m.latMax - m.latMin);
        const float centre = 0.5f * (m.latMax + m.latMin);
        float px = 0.0f, pz = 0.0f, rx = 0.0f, rz = 0.0f;
        map.PointAtArc(ramp.arcStart, px, pz);
        map.RightVectorAtArc(ramp.arcStart, rx, rz);
        ramp.entryX = px + rx * centre;
        ramp.entryZ = pz + rz * centre;
        g_ramps.push_back(ramp);
    }
    std::sort(g_ramps.begin(), g_ramps.end(),
              [](const Ramp& a, const Ramp& b) { return a.arcStart < b.arcStart; });
    RT_LOGF(RT_TAG_A11Y, "ramp map: %zu launch faces, %zu along the line, %zu ramps of %zu%s\n",
            g_faces.size(), placed.size(), g_ramps.size(), merged.size(),
            g_boundShifted ? " (shifted line)" : "");
    g_haveRamps = true;
    g_state = RampMapState::Done;
}

}  // namespace

void RampMap::Reset() {
    g_state = RampMapState::Idle;
    g_cursor = 1;
    g_faces.clear();
    g_ramps.clear();
    g_haveRamps = false;
    g_boundShifted = false;
    g_reachLeft.clear();
    g_reachRight.clear();
    g_placed.clear();
}

bool RampMap::Ready() { return g_haveRamps; }

const std::vector<Ramp>& RampMap::Ramps() { return g_ramps; }

void RampMap::Tick(const CourseMap& map) {
    if (!map.Loaded() || !map.RouteBased()) {
        return;
    }
    switch (g_state) {
        case RampMapState::Done:
            if (map.RoadShifted() != g_boundShifted) {
                StartBind(map);
            }
            return;
        case RampMapState::Idle:
            // After the edge sweep: it is what captures the mesh and settles the objects, and until
            // it is done the line may still be waiting to move.
            if (!EdgeMap::Ready() || !KclRoad::Capture()) {
                return;
            }
            g_state = RampMapState::Scanning;
            g_cursor = 1;
            g_faces.clear();
            return;
        case RampMapState::Scanning:
            if (!KclRoad::Capture()) {
                return;
            }
            g_cursor = KclScanCourseLaunch(g_cursor, kPrismsPerTick, g_faces);
            if (g_cursor <= KclMeshData().prismCount) {
                return;
            }
            if (KclObjects::Refresh()) {
                KclObjects::CollectLaunchFaces(g_faces);
            }
            StartBind(map);
            return;
        case RampMapState::Reaching: {
            const int stations = static_cast<int>(g_reachLeft.size());
            if (stations != map.StationCount()) {
                StartBind(map);
                return;
            }
            for (int done = 0; done < kReachStationsPerTick && g_reachCursor < stations; ++done) {
                g_reachLeft[g_reachCursor] = map.LateralReach(g_reachCursor, false);
                g_reachRight[g_reachCursor] = map.LateralReach(g_reachCursor, true);
                ++g_reachCursor;
            }
            if (g_reachCursor >= stations) {
                g_state = RampMapState::Placing;
            }
            return;
        }
        case RampMapState::Placing:
            for (std::size_t done = 0; done < kPlaceFacesPerTick && g_faceCursor < g_faces.size();
                 ++done, ++g_faceCursor) {
                PlacedFace p;
                if (PlaceFace(map, g_reachLeft, g_reachRight, g_faces[g_faceCursor], p)) {
                    g_placed.push_back(p);
                }
            }
            if (g_faceCursor >= g_faces.size()) {
                Finish(map);
            }
            return;
    }
}

}  // namespace a11y::race
