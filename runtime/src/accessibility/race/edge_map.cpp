#include "accessibility/race/edge_map.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <vector>

#include "accessibility/a11y_log.h"
#include "accessibility/race/course_map.h"
#include "accessibility/race/kcl_objects.h"
#include "accessibility/race/kcl_road.h"
#include "accessibility/race/race_manager.h"
#include "accessibility/race/racing_line.h"

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

enum class EdgeMapState { Idle, Running, Solving, Done };

EdgeMapState g_edgeMapState = EdgeMapState::Idle;
std::vector<StationEdges> g_edges;
// Places the line once the sweep is complete, one iteration per tick.
RacingLine g_line;
int g_cursor = 0;
int g_frames = 0;
int g_bothEdges = 0;
int g_fallEdges = 0;
// The course's real road scale, in world units. Anything measuring a length in track widths takes
// it from here: the KMP corridor is the CPU lane, not the road, and the two can differ by 14x.
float g_medianHalfWidth = 0.0f;
// Per station, towards the track's right: what it took to stand the line on asphalt. Mostly zero.
std::vector<float> g_shifts;
int g_shiftedStations = 0;
// Stations whose road is narrower than the kart's clearance on both sides together.
int g_tightStations = 0;
// Whether the course map has already said what it did with the shifts. Until it has, every measured
// distance reads from the authored stations, which is where the map still has them.
bool g_shiftConfirmed = false;
// Temporary. The worst single tick the build cost, so the next lap says whether the amortised
// probe is really invisible. Remove with the other cue diagnostics.
double g_worstTickMs = 0.0;

// Temporary. Whether the sweep found asphalt for each station's line point within the search limit
// it was given. The shift alone cannot separate "the route already stands on the road" from "the
// route is off the road and the limit was too small to reach it", and on a custom course whose KMP
// corridor is degenerate the second is the interesting one. Remove with the other cue diagnostics.
std::vector<std::uint8_t> g_lineOnRoad;

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

bool ProbeStation(const CourseMap& map, int station, StationEdges& out, bool& onRoadOut) {
    float cx = 0.0f, cz = 0.0f, rx = 0.0f, rz = 0.0f;
    map.Centre(station, cx, cz);
    map.RightVector(station, rx, rz);
    const float y = map.Height(station);

    // A route point can be authored off the asphalt - the item route follows what shells fly over,
    // and an ENPT stretch marked "only enter with an offroad-cutting item" is off the road on
    // purpose. The lateral sweep below starts one step OUT, so it never notices: it just returns a
    // zero-length road, the station is discarded, and the position term goes silent exactly where
    // the player most needs to hear that they are off the track. So the probe walks to the asphalt
    // first and measures from there.
    //
    // Bounded by the station's own KMP corridor: that is the game's own statement of how far the
    // line may legitimately sit from this point, so a correction inside it is still the route,
    // while a station needing more than that is a bad measurement rather than a bad line. A custom
    // course can author a corridor narrower than a single march step, though - 79 units on the
    // Retro Rewind course this was measured on - and the search then takes zero steps and repairs
    // nothing at all. One station spacing stands in there: it is the course's own length scale, and
    // a point needing to move further than the distance to its own neighbour is a bad point rather
    // than a misplaced line.
    float searchLimit = map.HalfWidth(station);
    if (searchLimit < kKclLateralStepUnits) {
        searchLimit = std::max(searchLimit, map.MeanSpacing());
    }
    float probeShift = 0.0f;
    onRoadOut = KclRoad::FindRoad(cx, y, cz, rx, rz, searchLimit, probeShift);
    // Each side stops where this station's own stretch of course does, so a paved infield joined to
    // the road cannot be measured as road (CourseMap::LateralReach). Taken about the authored point
    // and then carried across the repair: a probe that starts `probeShift` to the right has that
    // much less of the stretch left on its right and that much more on its left.
    const float leftReach = map.LateralReach(station, false) + probeShift;
    const float rightReach = map.LateralReach(station, true) - probeShift;
    const KclEdges edges = KclRoad::ProbeEdges(cx + rx * probeShift, y, cz + rz * probeShift, rx,
                                               rz, leftReach, rightReach);
    if (!edges.valid) {
        return false;
    }
    // Stated from the station the course AUTHORED, not from wherever the sweep stood, so the
    // line placement's answer is the TOTAL move. A side the authored point already stands outside
    // reads negative here, which is the truth about that point.
    out.leftKind = edges.left.valid ? ClassifyEdge(edges.left) : EdgeKind::Unknown;
    out.rightKind = edges.right.valid ? ClassifyEdge(edges.right) : EdgeKind::Unknown;
    out.leftDistance = edges.left.valid ? edges.left.distance - probeShift : 0.0f;
    out.rightDistance = edges.right.valid ? edges.right.distance + probeShift : 0.0f;
    // Placement is deliberately NOT done here: it needs every station. See BuildBands below and
    // RacingLine, which own `shift` outright.
    out.shift = 0.0f;
    // Each side stands on its own: a line point hard against one boundary still measured the other
    // correctly, and that is the side the player is about to need.
    out.leftValid = out.leftKind != EdgeKind::Unknown && out.leftDistance >= kEdgeMapMinDistance;
    out.rightValid = out.rightKind != EdgeKind::Unknown && out.rightDistance >= kEdgeMapMinDistance;
    return out.leftValid && out.rightValid;
}

// The line's placement, decided once every station has been swept.
//
// Each station gets a band it may stand in - the road on each side less a clearance - and the
// racing line is the line of least curvature through those bands (RacingLine). The clearance
// against a wall or the grass is the track-limit cue's own warning distance (never less than the
// kart's body), so the line runs as close to the limit as the player can be warned about - the
// player's spec, 2026-09-02, is Forza's: the line follows the best route even when that is near
// the limit. Placed at the kart's bare half-width it put "on the line" 103 units from the grass
// and the player off the road on a corner called gentle. A drop keeps the same share of its own
// local half-width clear as well. Only a side with a real edge bounds the band; an Unknown side
// pins the station where the course authored it, since nothing is known about the road there.
std::vector<LineBand> BuildBands(float kartHalfWidth) {
    const int count = static_cast<int>(g_edges.size());
    std::vector<LineBand> bands(static_cast<std::size_t>(count));
    g_tightStations = 0;
    for (int i = 0; i < count; ++i) {
        const StationEdges& e = g_edges[static_cast<std::size_t>(i)];
        LineBand& band = bands[static_cast<std::size_t>(i)];
        if (e.leftKind == EdgeKind::Unknown || e.rightKind == EdgeKind::Unknown) {
            continue;  // lo = hi = 0: stays where authored
        }
        const float half = (e.leftDistance + e.rightDistance) * 0.5f;
        auto clearance = [&](EdgeKind kind) {
            const float warned =
                std::max({EdgeMap::WarningDistance(), kartHalfWidth, kEdgeMapMinDistance});
            return kind == EdgeKind::Fall ? std::max(kEdgeOnsetRealFraction * half, warned)
                                          : warned;
        };
        band.lo = -e.leftDistance + clearance(e.leftKind);
        band.hi = e.rightDistance - clearance(e.rightKind);
        if (band.lo > band.hi) {
            ++g_tightStations;
        }
    }
    return bands;
}

// Gathered once the sweep is complete, in station order, for the course map to apply.
void CollectShifts() {
    g_shifts.assign(g_edges.size(), 0.0f);
    g_shiftedStations = 0;
    for (std::size_t i = 0; i < g_edges.size(); ++i) {
        g_shifts[i] = g_edges[i].shift;
        if (g_edges[i].shift != 0.0f) {
            ++g_shiftedStations;
        }
    }
}

// Over every measured side rather than per station, so one blind side does not discard the other.
void MeasureMedianHalfWidth() {
    std::vector<float> sides;
    sides.reserve(g_edges.size() * 2);
    for (const StationEdges& e : g_edges) {
        if (e.leftValid) { sides.push_back(e.leftDistance); }
        if (e.rightValid) { sides.push_back(e.rightDistance); }
    }
    g_medianHalfWidth = 0.0f;
    if (!sides.empty()) {
        std::nth_element(sides.begin(), sides.begin() + sides.size() / 2, sides.end());
        g_medianHalfWidth = sides[sides.size() / 2];
    }
}

void LogSummary(const CourseMap& map) {
    const int stations = static_cast<int>(g_edges.size());
    RT_LOGF(RT_TAG_A11Y,
            "edge map: %d stations, %d%% with both edges, %d%% fall-bounded, %d line stations repaired, %d object meshes "
            "(%d skipped: %d ptr, %d hdr), real half-width %.0f, worst tick %.1f ms\n",
            stations, stations > 0 ? g_bothEdges * 100 / stations : 0,
            stations > 0 ? g_fallEdges * 100 / stations : 0, g_shiftedStations, KclObjects::Count(),
            KclObjects::SkippedCount(), KclObjects::SkippedPointerCount(),
            KclObjects::SkippedHeaderCount(), static_cast<double>(g_medianHalfWidth),
            g_worstTickMs);
    // Temporary. "Centred" is the whole contract of the guide - the player's own words are that
    // with the engine centred they should be on the line to follow - so the one thing a log has to
    // be able to say is how far that line sits from the middle of the real asphalt. `toCentre` is
    // exactly the shift that would put a station between its two measured edges: zero means the
    // route already runs down the middle of the road, and a large median means a player obeying a
    // centred pan is being held off-centre all lap. Remove with the other cue diagnostics.
    std::vector<float> offsets;
    offsets.reserve(g_edges.size());
    int unrepairable = 0;
    int inWarningBand = 0;
    // Reported as the repair PROPOSES to leave the road, since the map has not yet said whether it
    // keeps the shift: the summary describes the line that is about to be offered, not the one the
    // sweep measured from.
    for (int i = 0; i < stations; ++i) {
        const StationEdges& e = g_edges[static_cast<std::size_t>(i)];
        const float left = e.leftDistance + e.shift;
        const float right = e.rightDistance - e.shift;
        if (i < static_cast<int>(g_lineOnRoad.size()) && g_lineOnRoad[static_cast<std::size_t>(i)] == 0) {
            ++unrepairable;
        }
        if (e.leftValid && e.rightValid) {
            offsets.push_back(std::fabs((right - left) * 0.5f));
        }
        // How many line points stand inside the track-limit cue's onset band: where the racing
        // line hugs a limit, and the edge cue speaks the moment the kart drifts off it.
        if (left > 0.0f && right > 0.0f) {
            const float half = (left + right) * 0.5f;
            if (std::min(left, right) < kEdgeOnsetRealFraction * half) {
                ++inWarningBand;
            }
        }
    }
    float medianOffset = 0.0f;
    if (!offsets.empty()) {
        std::nth_element(offsets.begin(), offsets.begin() + offsets.size() / 2, offsets.end());
        medianOffset = offsets[offsets.size() / 2];
    }
    RT_LOGF(RT_TAG_A11Y,
            "edge centring: median off-centre %.0f of half-width %.0f (%d%%), %d stations hugging "
            "a limit, %d narrower than the kart, %d off road the repair could not "
            "reach, kmp median half-width %.0f\n",
            static_cast<double>(medianOffset), static_cast<double>(g_medianHalfWidth),
            g_medianHalfWidth > 0.0f ? static_cast<int>(medianOffset * 100.0f / g_medianHalfWidth) : 0,
            inWarningBand, g_tightStations, unrepairable,
            static_cast<double>(map.MedianHalfWidth()));
    // Temporary. One row per station in the same columns and the same raw world units as the
    // offline reference table, so a lap's log diffs straight against it. `to-centre` is what this
    // station would have to move to sit mid-road, `kmp` is the corridor that bounds the repair
    // search, and `off-road` marks a point the search never reached asphalt from. Remove with the
    // other cue diagnostics once the onset fraction is calibrated.
    for (int i = 0; i < stations; ++i) {
        const StationEdges& e = g_edges[static_cast<std::size_t>(i)];
        const float left = e.leftDistance + e.shift;
        const float right = e.rightDistance - e.shift;
        const bool onRoad = i >= static_cast<int>(g_lineOnRoad.size()) ||
                            g_lineOnRoad[static_cast<std::size_t>(i)] != 0;
        RT_LOGF(RT_TAG_A11Y,
                "edge row: %d | %.0f | %s | %.0f | %s | to-centre %.0f | shift %.0f | kmp %.0f | %s\n",
                i, static_cast<double>(left), EdgeKindName(e.leftKind),
                static_cast<double>(right), EdgeKindName(e.rightKind),
                static_cast<double>((right - left) * 0.5f), static_cast<double>(e.shift),
                static_cast<double>(map.HalfWidth(i)), onRoad ? "on-road" : "off-road");
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
    g_medianHalfWidth = 0.0f;
    g_worstTickMs = 0.0;
    g_lineOnRoad.clear();
    g_shifts.clear();
    g_shiftedStations = 0;
    g_tightStations = 0;
    g_shiftConfirmed = false;
    KclObjects::Forget();
    // The course mesh too: its header is cached against a controller pointer that a new course can
    // land on again, and this is the one place that knows the course is gone.
    KclRoad::Forget();
}

bool EdgeMap::Ready() { return g_edgeMapState == EdgeMapState::Done && !g_edges.empty(); }

float EdgeMap::MedianHalfWidth() { return g_medianHalfWidth; }

float EdgeMap::WarningDistance() { return kEdgeOnsetRealFraction * g_medianHalfWidth; }

const std::vector<float>& EdgeMap::Shifts() { return g_shifts; }

void EdgeMap::ConfirmShift(bool applied) {
    if (g_shiftConfirmed) {
        return;
    }
    g_shiftConfirmed = true;
    if (!applied) {
        return;  // the stations never moved, and the distances already read from where they are
    }
    for (StationEdges& e : g_edges) {
        // Analytically rather than by a second sweep: between two measured boundaries the road is
        // the straight line this module already treats it as.
        e.leftDistance += e.shift;
        e.rightDistance -= e.shift;
        e.leftValid = e.leftKind != EdgeKind::Unknown && e.leftDistance >= kEdgeMapMinDistance;
        e.rightValid = e.rightKind != EdgeKind::Unknown && e.rightDistance >= kEdgeMapMinDistance;
    }
    MeasureMedianHalfWidth();  // the repair changed which sides read, so the scale is restated
}

bool EdgeMap::AnyShift() { return g_shiftedStations > 0; }

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

void EdgeMap::Tick(const CourseMap& map, float kartHalfWidth) {
    if (g_edgeMapState == EdgeMapState::Done) {
        return;
    }
    if (g_edgeMapState == EdgeMapState::Solving) {
        if (static_cast<int>(g_edges.size()) != map.StationCount()) {
            Reset();
            return;
        }
        const auto started = std::chrono::steady_clock::now();
        const bool placed = g_line.Step();
        g_worstTickMs = std::max(
            g_worstTickMs,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count());
        if (!placed) {
            return;
        }
        const std::vector<float>& shifts = g_line.Shifts();
        for (std::size_t i = 0; i < g_edges.size() && i < shifts.size(); ++i) {
            g_edges[i].shift = shifts[i];
        }
        g_edgeMapState = EdgeMapState::Done;
        CollectShifts();
        MeasureMedianHalfWidth();
        RT_LOGF(RT_TAG_A11Y, "edge map: line placed in %d iterations, %d stations on a limit\n",
                g_line.Iterations(), g_line.Pinned());
        LogSummary(map);
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
        g_lineOnRoad.assign(static_cast<std::size_t>(map.StationCount()), 0);
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
        bool onRoad = false;
        const int station = g_cursor++;
        if (ProbeStation(map, station, edges, onRoad)) {
            ++g_bothEdges;
            if (edges.leftKind == EdgeKind::Fall || edges.rightKind == EdgeKind::Fall) {
                ++g_fallEdges;
            }
        }
        g_edges[static_cast<std::size_t>(station)] = edges;
        g_lineOnRoad[static_cast<std::size_t>(station)] = onRoad ? 1 : 0;
    }
    g_worstTickMs = std::max(
        g_worstTickMs,
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count());
    if (g_cursor >= map.StationCount()) {
        // The bands need the kart's size, which reads only while a kart exists; until then the
        // sweep stays complete and waits.
        if (!(kartHalfWidth > 0.0f)) {
            return;
        }
        g_edgeMapState = EdgeMapState::Solving;
        MeasureMedianHalfWidth();  // the bands need the warning distance, which needs the scale
        // Settled once no station wants to move by a probe step: the edges themselves are not
        // known any finer than that.
        g_line.Begin(map, BuildBands(kartHalfWidth), kKclLateralStepUnits);
        RT_LOGF(RT_TAG_A11Y, "edge map: placing the line, kart half-width %.0f\n",
                static_cast<double>(kartHalfWidth));
    }
}

}  // namespace a11y::race
