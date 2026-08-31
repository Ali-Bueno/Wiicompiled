#include "accessibility/race/edge_map.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
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
// The course's real road scale, in world units. Anything measuring a length in track widths takes
// it from here: the KMP corridor is the CPU lane, not the road, and the two can differ by 14x.
float g_medianHalfWidth = 0.0f;
// Per station, towards the track's right: what it took to stand the line on asphalt. Mostly zero.
std::vector<float> g_shifts;
int g_shiftedStations = 0;
// Stations where the safe band and the step limit could not both be honoured - the road moves
// sideways faster than the line is allowed to follow. Zero on every course measured so far.
int g_tightStations = 0;
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
    // while a station needing more than that is a bad measurement rather than a bad line.
    float shift = 0.0f;
    onRoadOut = KclRoad::FindRoad(cx, y, cz, rx, rz, map.HalfWidth(station), shift);
    // Each side stops where this station's own stretch of course does, so a paved infield joined to
    // the road cannot be measured as road (CourseMap::LateralReach). Taken about the authored point
    // and then carried across the repair: a probe that starts `shift` to the right has that much
    // less of the stretch left on its right and that much more on its left.
    const float leftReach = map.LateralReach(station, false) + shift;
    const float rightReach = map.LateralReach(station, true) - shift;
    const KclEdges edges = KclRoad::ProbeEdges(cx + rx * shift, y, cz + rz * shift, rx, rz,
                                               leftReach, rightReach);
    if (!edges.valid) {
        return false;
    }
    out.leftKind = ClassifyEdge(edges.left);
    out.rightKind = ClassifyEdge(edges.right);
    out.leftDistance = edges.left.distance;
    out.rightDistance = edges.right.distance;
    // Centring is deliberately NOT done here: it needs the neighbouring stations, which are not
    // measured yet. See CentreLine below.
    out.shift = shift;
    // Each side stands on its own: a line point hard against one boundary still measured the other
    // correctly, and that is the side the player is about to need.
    out.leftValid = out.leftDistance >= kEdgeMapMinDistance;
    out.rightValid = out.rightDistance >= kEdgeMapMinDistance;
    return out.leftValid && out.rightValid;
}

// The line repair's second pass, run once every station has been swept.
//
// A route point that stands hard against one edge is a point in the wrong place, and the guide must
// not call it "centred": the player's spec is that the centre of the pan is the line they can drive
// without hitting anything, on any track, in a corner as much as on a straight. Measured on the
// course they reported, at the entry to its first corner, the line jogs ~1,000 units sideways and
// stands 150 units from the right-hand edge while the total road there is 2,300 - the same as at
// its neighbours. The road does not pinch; the line does. Obeying a centred pan into a left-hander
// from there carries the kart straight off.
//
// This does not re-author the racing line. It is the SMALLEST move that puts every station inside
// its own safe band, and a station already safe does not move at all - which matters, because a
// wholesale KCL-recentred line lost to the authored one on 7 of 8 offline courses
// (route_graph.cpp). Three derived quantities and no tunables:
//
//   band  Where the track-limit cue is silent: kEdgeOnsetRealFraction of the local half-width clear
//         on each side. The same constant, so "centred" and "not warning" are one statement. The
//         band is always exactly one half-width wide and always contains the road's own centre, so
//         on its own it can never be empty.
//   free  An apron or a junction measures a wide open space that is not a lane, and centring
//         station 5 of that course by 1,150 units put the player off a bridge approach while they
//         sat exactly on the line. An apron is a LOCAL MAXIMUM of road width, which takes no
//         constant to recognise. Those stations, and any the sweep could not read, get no band:
//         they neither demand a move nor block one, so a correction can ramp across them.
//   step  A lateral step d between stations spaced s apart turns the line by about 2*atan(d/s), and
//         the curve pass reads anything below kTightnessEnter as straight, so neighbouring shifts
//         differing by at most s*tan(kTightnessEnter/2) can never be graded as a corner. The first
//         version put this cap on the shift ITSELF, which limited how FAR the repair could reach
//         instead of how FAST: a lone station could not take its neighbours with it, so that corner
//         entry ended at 450 units of margin rather than clear of the band. Bounding the step lets
//         a whole stretch slide as one.
//
// The bands are propagated around the loop until they stop changing, which folds the step limit
// into them. After that each station's answer is just the point of its band nearest to not moving
// at all, and that pick is automatically within one step of its neighbours' because clamping is
// 1-Lipschitz in the bounds it clamps to - so the shape of the course is preserved by construction
// rather than by dilution. A band that propagation empties is a place where the road moves sideways
// faster than the line may follow: those take the midpoint, are counted, and ApplyRoadShift's
// corner check stays as the backstop.
//
// The edges follow the move analytically rather than by a second sweep: between two measured
// boundaries the road is the straight line this module already treats it as.
void CentreLine(const CourseMap& map) {
    const int count = static_cast<int>(g_edges.size());
    g_tightStations = 0;
    if (count < 3) {
        return;
    }
    const float maxStep = map.MeanSpacing() * std::tan(kTightnessEnter * 0.5f);
    constexpr float kFree = std::numeric_limits<float>::infinity();

    // Every decision reads the road as SWEPT, so a station already moved cannot change what its
    // neighbour thinks the road there is.
    std::vector<float> halfWidth(static_cast<std::size_t>(count), 0.0f);
    for (int i = 0; i < count; ++i) {
        const StationEdges& e = g_edges[static_cast<std::size_t>(i)];
        halfWidth[static_cast<std::size_t>(i)] = (e.leftDistance + e.rightDistance) * 0.5f;
    }

    std::vector<float> lo(static_cast<std::size_t>(count), -kFree);
    std::vector<float> hi(static_cast<std::size_t>(count), kFree);
    for (int i = 0; i < count; ++i) {
        const StationEdges& e = g_edges[static_cast<std::size_t>(i)];
        if (!(e.leftDistance > 0.0f) || !(e.rightDistance > 0.0f)) {
            continue;
        }
        const float here = halfWidth[static_cast<std::size_t>(i)];
        const float prev = halfWidth[static_cast<std::size_t>((i + count - 1) % count)];
        const float next = halfWidth[static_cast<std::size_t>((i + 1) % count)];
        if (here > prev && here > next) {
            continue;
        }
        const float clear = kEdgeOnsetRealFraction * here;
        lo[static_cast<std::size_t>(i)] = -e.leftDistance + clear;
        hi[static_cast<std::size_t>(i)] = e.rightDistance - clear;
    }

    // Both chains are monotone - lo only rises, hi only falls - and bounded by the widest band, so
    // this converges; a lap that changes nothing is the fixed point. Forward and backward both run
    // in every pass, because it takes both to make neighbouring bounds differ by at most one step,
    // which is what the pick below relies on.
    for (int pass = 0; pass < count; ++pass) {
        bool changed = false;
        for (int i = 0; i < count; ++i) {
            const std::size_t here = static_cast<std::size_t>(i);
            const std::size_t back = static_cast<std::size_t>((i + count - 1) % count);
            const float raised = lo[back] - maxStep;
            const float lowered = hi[back] + maxStep;
            if (raised > lo[here]) { lo[here] = raised; changed = true; }
            if (lowered < hi[here]) { hi[here] = lowered; changed = true; }
        }
        for (int i = count - 1; i >= 0; --i) {
            const std::size_t here = static_cast<std::size_t>(i);
            const std::size_t ahead = static_cast<std::size_t>((i + 1) % count);
            const float raised = lo[ahead] - maxStep;
            const float lowered = hi[ahead] + maxStep;
            if (raised > lo[here]) { lo[here] = raised; changed = true; }
            if (lowered < hi[here]) { hi[here] = lowered; changed = true; }
        }
        if (!changed) {
            break;
        }
    }

    for (int i = 0; i < count; ++i) {
        StationEdges& e = g_edges[static_cast<std::size_t>(i)];
        const float low = lo[static_cast<std::size_t>(i)];
        const float high = hi[static_cast<std::size_t>(i)];
        float shift = 0.0f;
        if (low > high) {
            shift = (low + high) * 0.5f;  // both ends finite: a free station's band never empties
            ++g_tightStations;
        } else {
            shift = std::max(low, std::min(high, 0.0f));
        }
        if (!std::isfinite(shift)) {
            continue;
        }
        // The measured sides bound the move too, for the empty-band case that ignored the band.
        if (e.leftDistance > 0.0f && e.rightDistance > 0.0f) {
            const float floorShift = -e.leftDistance + kEdgeMapMinDistance;
            const float ceilShift = e.rightDistance - kEdgeMapMinDistance;
            if (floorShift <= ceilShift) {
                shift = std::max(floorShift, std::min(ceilShift, shift));
            }
            e.leftDistance += shift;
            e.rightDistance -= shift;
        }
        // Recorded even where the sides did not read: the shift is what keeps the line continuous,
        // and a free station dropping its ramp back to zero is the step the cap exists to prevent.
        e.shift += shift;
    }
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
    for (int i = 0; i < stations; ++i) {
        const StationEdges& e = g_edges[static_cast<std::size_t>(i)];
        if (i < static_cast<int>(g_lineOnRoad.size()) && g_lineOnRoad[static_cast<std::size_t>(i)] == 0) {
            ++unrepairable;
        }
        if (e.leftValid && e.rightValid) {
            offsets.push_back(std::fabs((e.rightDistance - e.leftDistance) * 0.5f));
        }
        // The player's spec, counted directly: after the repair, how many line points still stand
        // closer to an edge than the track-limit cue's own onset. Anything but zero is a place
        // where a centred pan does not mean a place the kart fits.
        if (e.leftDistance > 0.0f && e.rightDistance > 0.0f) {
            const float half = (e.leftDistance + e.rightDistance) * 0.5f;
            if (std::min(e.leftDistance, e.rightDistance) < kEdgeOnsetRealFraction * half) {
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
            "edge centring: median off-centre %.0f of half-width %.0f (%d%%), %d stations still "
            "inside the warning band, %d too tight to place, %d off road the repair could not "
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
        const bool onRoad = i >= static_cast<int>(g_lineOnRoad.size()) ||
                            g_lineOnRoad[static_cast<std::size_t>(i)] != 0;
        RT_LOGF(RT_TAG_A11Y,
                "edge row: %d | %.0f | %s | %.0f | %s | to-centre %.0f | shift %.0f | kmp %.0f | %s\n",
                i, static_cast<double>(e.leftDistance), EdgeKindName(e.leftKind),
                static_cast<double>(e.rightDistance), EdgeKindName(e.rightKind),
                static_cast<double>((e.rightDistance - e.leftDistance) * 0.5f),
                static_cast<double>(e.shift), static_cast<double>(map.HalfWidth(i)),
                onRoad ? "on-road" : "off-road");
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
    KclObjects::Forget();
    // The course mesh too: its header is cached against a controller pointer that a new course can
    // land on again, and this is the one place that knows the course is gone.
    KclRoad::Forget();
}

bool EdgeMap::Ready() { return g_edgeMapState == EdgeMapState::Done && !g_edges.empty(); }

float EdgeMap::MedianHalfWidth() { return g_medianHalfWidth; }

const std::vector<float>& EdgeMap::Shifts() { return g_shifts; }

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
        g_edgeMapState = EdgeMapState::Done;
        CentreLine(map);
        CollectShifts();
        MeasureMedianHalfWidth();
        LogSummary(map);
    }
}

}  // namespace a11y::race
