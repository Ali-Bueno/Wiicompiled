#include "accessibility/race/edge_map.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <vector>

#include "accessibility/a11y_log.h"
#include "accessibility/race/course_map.h"
#include "accessibility/race/kcl_objects.h"
#include "accessibility/race/kcl_road.h"
#include "accessibility/race/race_manager.h"

namespace a11y::race {
namespace {

// Stations measured per tick. A station costs two sweeps of up to 160 probes each and every probe
// re-decodes a leaf's prisms, so the whole map in one frame is a visible freeze. Spread this thin
// the build is background work: a 40-station course is measured within the countdown, and the
// longest route finishes in a few seconds of racing.
constexpr int kEdgeMapStationsPerTick = 2;

// How long to keep waiting for the collision mesh before accepting there is none - the same
// schedule the route retry gives up on, so a course that never reads says so once.
constexpr int kEdgeMapSettleFrames = kCourseSettleFrames;

// A side whose road is narrower than one march step is a probe that went wrong, not a road - the
// march cannot resolve anything finer than its own step.
constexpr float kEdgeMapMinDistance = kKclLateralStepUnits;

enum class EdgeMapState { Idle, Running, Done };

EdgeMapState g_edgeMapState = EdgeMapState::Idle;
std::vector<StationEdges> g_edges;
int g_cursor = 0;
int g_frames = 0;
int g_bothEdges = 0;
int g_fallEdges = 0;
// Temporary. The worst single tick the build cost, so the next lap says whether the amortised
// probe is really invisible. Remove with the other cue diagnostics.
double g_worstTickMs = 0.0;

// Classified by what lies BEYOND the boundary, never by the prism that forms it. The mushroom caps
// end in a 0x1E SpecialWall rim with void past it - a drop the player goes over - and a real 0x0C
// cliff reads the same way, so the prism type alone called 11 of kinoko's 13 fall-bounded stations
// a wall. An offroad shoulder with nothing behind it is a fall too.
EdgeKind ClassifyEdge(const KclEdge& edge) {
    if (edge.openEnded) {
        return EdgeKind::Open;
    }
    if (!edge.beyondHit) {
        return EdgeKind::Fall;  // nothing within a probe reach out there: it is a drop
    }
    switch (edge.beyond) {
        case KclSurface::Fall:
            return EdgeKind::Fall;
        case KclSurface::Offroad:
            return EdgeKind::Offroad;
        case KclSurface::Road:
            // Road still behind the boundary, so it is something to scrape along - unless the
            // boundary itself is a fall surface, which outranks what is behind it.
            return edge.cause == KclSurface::Fall ? EdgeKind::Fall : EdgeKind::Wall;
        default:
            return EdgeKind::Fall;
    }
}

bool ProbeStation(const CourseMap& map, int station, StationEdges& out) {
    float cx = 0.0f, cz = 0.0f, rx = 0.0f, rz = 0.0f;
    map.Centre(station, cx, cz);
    map.RightVector(station, rx, rz);
    const KclEdges edges = KclRoad::ProbeEdges(cx, map.Height(station), cz, rx, rz);
    if (!edges.valid) {
        return false;
    }
    out.leftKind = ClassifyEdge(edges.left);
    out.rightKind = ClassifyEdge(edges.right);
    out.leftDistance = edges.left.distance;
    out.rightDistance = edges.right.distance;
    // Each side stands on its own: a line point hard against one boundary still measured the other
    // correctly, and that is the side the player is about to need.
    out.leftValid = out.leftDistance >= kEdgeMapMinDistance;
    out.rightValid = out.rightDistance >= kEdgeMapMinDistance;
    return out.leftValid && out.rightValid;
}

void LogSummary() {
    const int stations = static_cast<int>(g_edges.size());
    RT_LOGF(RT_TAG_A11Y,
            "edge map: %d stations, %d%% with both edges, %d%% fall-bounded, %d object meshes "
            "(%d skipped: %d ptr, %d hdr), worst tick %.1f ms\n",
            stations, stations > 0 ? g_bothEdges * 100 / stations : 0,
            stations > 0 ? g_fallEdges * 100 / stations : 0, KclObjects::Count(),
            KclObjects::SkippedCount(), KclObjects::SkippedPointerCount(),
            KclObjects::SkippedHeaderCount(), g_worstTickMs);
    // Temporary. One row per station in the same columns and the same raw world units as the
    // offline reference table, so a lap's log diffs straight against it. Remove with the other cue
    // diagnostics once the onset fraction is calibrated.
    for (int i = 0; i < stations; ++i) {
        const StationEdges& e = g_edges[static_cast<std::size_t>(i)];
        RT_LOGF(RT_TAG_A11Y, "edge row: %d | %.0f | %s | %.0f | %s\n", i,
                static_cast<double>(e.leftDistance), EdgeKindName(e.leftKind),
                static_cast<double>(e.rightDistance), EdgeKindName(e.rightKind));
    }
}

}  // namespace

const char* EdgeKindName(EdgeKind kind) {
    switch (kind) {
        case EdgeKind::Open:
            return "open";
        case EdgeKind::Offroad:
            return "offroad";
        case EdgeKind::Wall:
            return "wall";
        case EdgeKind::Fall:
            return "fall";
        default:
            return "unknown";
    }
}

void EdgeMap::Reset() {
    g_edgeMapState = EdgeMapState::Idle;
    g_edges.clear();
    g_cursor = 0;
    g_frames = 0;
    g_bothEdges = 0;
    g_fallEdges = 0;
    g_worstTickMs = 0.0;
    KclObjects::Forget();
    // The course mesh too: its header is cached against a controller pointer that a new course can
    // land on again, and this is the one place that knows the course is gone.
    KclRoad::Forget();
}

bool EdgeMap::Ready() { return g_edgeMapState == EdgeMapState::Done && !g_edges.empty(); }

StationEdges EdgeMap::At(int station) {
    if (station < 0 || station >= static_cast<int>(g_edges.size())) {
        return StationEdges{};
    }
    return g_edges[static_cast<std::size_t>(station)];
}

bool EdgeMap::SideAtArc(const CourseMap& map, float arc, bool right, float& distance,
                        EdgeKind& kind) {
    int station = 0;
    float t = 0.0f;
    const int count = static_cast<int>(g_edges.size());
    if (count <= 0 || !map.SegmentAtArc(arc, station, t) || station < 0 || station >= count) {
        return false;
    }
    const StationEdges& a = g_edges[static_cast<std::size_t>(station)];
    const StationEdges& b = g_edges[static_cast<std::size_t>((station + 1) % count)];
    if (!a.Valid(right) || !b.Valid(right)) {
        return false;
    }
    const float da = a.Distance(right);
    const float db = b.Distance(right);
    distance = da + (db - da) * t;
    kind = (a.Kind(right) == EdgeKind::Fall || b.Kind(right) == EdgeKind::Fall)
               ? EdgeKind::Fall
               : (t < 0.5f ? a.Kind(right) : b.Kind(right));
    return distance > 0.0f;
}

void EdgeMap::Tick(const CourseMap& map) {
    if (g_edgeMapState == EdgeMapState::Done) {
        return;
    }
    // The heights come from the route points, so the checkpoint fallback has none to probe from.
    if (!map.Loaded() || !map.RouteBased()) {
        return;
    }
    if (!KclRoad::Capture()) {
        if (++g_frames >= kEdgeMapSettleFrames) {
            g_edgeMapState = EdgeMapState::Done;  // no mesh: every station stays Unknown and the cue
            RT_LOGF(RT_TAG_A11Y,          // falls back to the corridor rather than going quiet
                    "edge map: no collision mesh after %d frames, corridor cues only\n", g_frames);
        }
        return;
    }
    // The object meshes are revalidated alongside, because the caps are what the gorge crossing
    // stands on. Sampled at whatever phase a moving object happens to be in: a per-course map
    // cannot follow kinoko's rising mushrooms, and rebuilding it per frame to chase them would
    // cost far more than it buys.
    KclObjects::Refresh();

    if (g_edgeMapState == EdgeMapState::Idle) {
        g_edgeMapState = EdgeMapState::Running;
        g_edges.assign(static_cast<std::size_t>(map.StationCount()), StationEdges{});
        g_cursor = 0;
        // Temporary. Logged at the START as well as at the end, because the build takes only a
        // few tenths of a second and the objects are still loading during it: a count that grows
        // between the two lines means the map was probed against a half-built object list, which
        // no retry can fix afterwards - Tick stops calling Refresh once the build is Done.
        RT_LOGF(RT_TAG_A11Y,
                "edge map: start, %d stations, %d object meshes (%d skipped: %d ptr, %d hdr)\n",
                map.StationCount(), KclObjects::Count(), KclObjects::SkippedCount(),
                KclObjects::SkippedPointerCount(), KclObjects::SkippedHeaderCount());
    }
    // A station count that moved under a running build would index another course's geometry.
    if (static_cast<int>(g_edges.size()) != map.StationCount()) {
        Reset();
        return;
    }

    ++g_frames;
    // Temporary: the probe is only background work if it really is, and only a clock says so.
    const auto started = std::chrono::steady_clock::now();
    for (int done = 0; done < kEdgeMapStationsPerTick && g_cursor < map.StationCount(); ++done) {
        StationEdges edges;
        const int station = g_cursor++;
        if (ProbeStation(map, station, edges)) {
            ++g_bothEdges;
            if (edges.leftKind == EdgeKind::Fall || edges.rightKind == EdgeKind::Fall) {
                ++g_fallEdges;
            }
        }
        g_edges[static_cast<std::size_t>(station)] = edges;
    }
    g_worstTickMs = std::max(
        g_worstTickMs,
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count());
    if (g_cursor >= map.StationCount()) {
        g_edgeMapState = EdgeMapState::Done;
        LogSummary();
    }
}

}  // namespace a11y::race
