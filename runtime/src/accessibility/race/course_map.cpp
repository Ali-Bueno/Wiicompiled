#include "accessibility/race/course_map.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "accessibility/a11y_log.h"
#include "accessibility/race/edge_map.h"
#include "accessibility/race/kmp_reader.h"

namespace a11y::race {
namespace {

// The nearest-station search is limited to this fraction of the lap either side of the last known
// station. A kart cannot cross a quarter of a lap in one frame, and on a course that crosses over
// itself the two branches are far apart in lap distance even where they are close in space.
constexpr float kSearchArcFraction = 0.25f;

// Sanity band for (derived lap / game's own lap). Offline, both backbones stay within 0.60-1.08
// of even a naive checkpoint reference across all 32 courses; what this rejects is a walk that
// closed a shortcut side-loop (a fraction of the lap) or doubled the path (2.07x, the original
// ENPT-by-index bug). Outside the band the route is rejected so the caller can try another source.
constexpr float kLapRatioMin = 0.5f;
constexpr float kLapRatioMax = 2.0f;

// Both ends of a forward blend are unit vectors, so the blend is one unit long when they agree and
// shrinks to zero as they oppose. A thousandth of that is a heading the lerp no longer states: the
// two stations face within a rounding error of exactly opposite, and normalising past this point
// amplifies float noise into a confident direction.
constexpr float kForwardBlendMinLength = 1e-3f;

float Hypot2(float dx, float dz) {
    return std::sqrt(dx * dx + dz * dz);
}

}  // namespace

int CourseMap::Wrap(int i) const {
    const int n = static_cast<int>(mPoints.size());
    if (n <= 0) {
        return 0;
    }
    const int m = i % n;
    return m < 0 ? m + n : m;
}

void CourseMap::Clear() {
    mGraph.Clear();
    mPoints.clear();
    mArc.clear();
    mLapVertices.clear();
    mVertexArc.clear();
    mVertexArcStep = 0.0f;
    mStationForCheckpoint.clear();
    mCurves.clear();
    ++mCurveGeneration;
    mLapLength = 0.0f;
    mMeanSpacing = 0.0f;
    mMedianHalfWidth = 0.0f;
    mRightPerpSign = 1.0f;
    mHintWindow = 2;
    mRouteBased = false;
    mRoadShifted = false;
}

void CourseMap::Centre(int i, float& x, float& z) const {
    x = 0.0f;
    z = 0.0f;
    if (mPoints.empty()) {
        return;
    }
    const Station& s = mPoints[Wrap(i)];
    x = s.x;
    z = s.z;
}

float CourseMap::HalfWidth(int i) const {
    return mPoints.empty() ? 0.0f : mPoints[Wrap(i)].halfWidth;
}

float CourseMap::Height(int i) const {
    return mPoints.empty() ? 0.0f : mPoints[Wrap(i)].y;
}

float CourseMap::ArcAt(int i) const {
    return mArc.empty() ? 0.0f : mArc[Wrap(i)];
}

int CourseMap::StationForCheckpoint(int checkpoint) const {
    if (checkpoint < 0 || checkpoint >= static_cast<int>(mStationForCheckpoint.size())) {
        return -1;
    }
    return mStationForCheckpoint[checkpoint];
}

void CourseMap::Forward(int i, float& x, float& z) const {
    float ax, az, bx, bz;
    Centre(i - 1, ax, az);
    Centre(i + 1, bx, bz);
    x = bx - ax;
    z = bz - az;
    const float len = Hypot2(x, z);
    if (len > 0.0f) {
        x /= len;
        z /= len;
    }
}

void CourseMap::RightVector(int i, float& x, float& z) const {
    float fx, fz;
    Forward(i, fx, fz);
    x = fz * mRightPerpSign;
    z = -fx * mRightPerpSign;
}

void CourseMap::RightVectorAtArc(float arc, float& x, float& z) const {
    float fx, fz;
    ForwardAtArc(arc, fx, fz);
    x = fz * mRightPerpSign;
    z = -fx * mRightPerpSign;
}

// A lateral sweep that only stops when the surface stops being road walks off this stretch of
// course entirely wherever the route doubles back and the ground between the two passes is paved:
// the infield of an oval, the inside of a hairpin. Measured on a Retro Rewind N64 course, the
// sweep reported 3100-8000 units of "road" on one side against 500-3500 on the other, eight
// stations reaching the 8000-unit probe cap without finding an edge, and the wide side swapping
// from left to right half way round the lap - the signature of crossing to the opposite straight.
// Everything the guide measures in road widths then rides on a road several times too wide.
//
// The stopping rule is the route's own medial axis, which is why it needs no constant: the sample
// is this station's for as long as this station's point is the nearest one to it. Walking out along
// +/-right, station j takes over where the perpendicular bisector of the two points crosses the
// ray, at |d|^2 / (2*dot(d, r)) with d = P_j - P_i; only a station the ray actually closes on
// (dot > 0) can ever take over.
//
// It cannot truncate a genuinely wide road. A neighbour along the line is offset forwards, so its
// dot is ~0 and its crossing is effectively at infinity - purely sideways motion never hands the
// sample away. On the inside of a bend the nearest crossing is the arc's own centre, one turn
// radius out, and a road whose half-width reached that far would have no inside edge at all.
float CourseMap::LateralReach(int station, bool right) const {
    const int count = StationCount();
    if (count <= 0) {
        return 0.0f;
    }
    const int i = Wrap(station);
    float rx = 0.0f, rz = 0.0f;
    RightVector(i, rx, rz);
    if (!right) {
        rx = -rx;
        rz = -rz;
    }
    const float px = mPoints[i].x;
    const float pz = mPoints[i].z;
    // The lap is the course's own largest length scale, so an unbounded ray still returns a real
    // number and the caller never has to know about a sentinel.
    float reach = mLapLength;
    for (int j = 0; j < count; ++j) {
        if (j == i) {
            continue;
        }
        const float dx = mPoints[j].x - px;
        const float dz = mPoints[j].z - pz;
        const float along = dx * rx + dz * rz;
        if (!(along > 0.0f)) {
            continue;
        }
        const float crossing = (dx * dx + dz * dz) / (2.0f * along);
        if (crossing < reach) {
            reach = crossing;
        }
    }
    return reach;
}

bool CourseMap::Build(std::vector<RoutePoint> route, std::uint8_t startPoint,
                      const std::vector<Checkpoint>& checkpoints) {
    Clear();
    if (static_cast<int>(checkpoints.size()) < kMinStations) {
        return false;
    }

    mGraph.Build(std::move(route), startPoint);
    const bool routeLap = BuildRouteStations();
    if (routeLap) {
        ResampleUniform();
    } else {
        BuildCheckpointStations(checkpoints);
    }
    if (!Loaded()) {
        return false;
    }

    BuildDerived();

    // Against the game's own lap length, and with teeth now that the caller has a second source
    // to fall back to: a route walk can close a CYCLE that is not the LAP (a shortcut side-loop,
    // or the doubled path of the original by-index bug), and only this comparison can tell.
    bool lapSane = true;
    float gameLap = 0.0f;
    if (ReadCourseLapLength(gameLap)) {
        const float ratio = mLapLength / gameLap;
        // The KMP half-width is what the aim distance scales by; the edge map logs the real road
        // beside it, and the two only agree by accident. Temporary, with the other diagnostics.
        RT_LOGF(RT_TAG_A11Y,
                "course map: %d stations, lap %.0f, game says %.0f (%.2fx), kmp half-width %.0f\n",
                StationCount(), static_cast<double>(mLapLength), static_cast<double>(gameLap),
                static_cast<double>(ratio), static_cast<double>(mMedianHalfWidth));
        lapSane = ratio >= kLapRatioMin && ratio <= kLapRatioMax;
        if (routeLap && !lapSane) {
            RT_LOGF(RT_TAG_A11Y,
                    "course map: WARNING lap ratio %.2f outside sanity band, rejecting route\n",
                    static_cast<double>(ratio));
        }
    }

    BuildCheckpointMap(checkpoints);
    // Corner sides read the right vector, which BuildCheckpointMap settles, so this has to run
    // after it.
    BuildCurves();
    mRouteBased = routeLap && lapSane;
    return mRouteBased;
}

// One station per median corridor half-width, the lap's own length scale: fine enough that a
// corner spans several stations wherever the road could hold one, and the same on every course.
// The authored points are a route, not a sampling - Mushroom Gorge's are 5,000 units apart on a
// 750-unit corridor - and every threshold stated per station meant a different thing per course.
// Station zero stays the route's start point so the checkpoint mapping is unchanged.
void CourseMap::ResampleUniform() {
    const int n = StationCount();
    if (n < kMinStations) {
        return;
    }
    std::vector<float> arc(static_cast<std::size_t>(n) + 1, 0.0f);
    for (int i = 0; i < n; ++i) {
        const Station& a = mPoints[static_cast<std::size_t>(i)];
        const Station& b = mPoints[static_cast<std::size_t>(Wrap(i + 1))];
        arc[static_cast<std::size_t>(i) + 1] = arc[static_cast<std::size_t>(i)] + Hypot2(b.x - a.x, b.z - a.z);
    }
    const float lap = arc.back();

    // The spacing is the lap's median corridor half-width. A route authored with no corridor at
    // all (11 of Retro Rewind's 341 tracks leave every ENPT range at 0) falls back to the route's
    // own median segment length - still the course's scale, and the corners need no width.
    std::vector<float> scale;
    scale.reserve(static_cast<std::size_t>(n));
    for (const Station& s : mPoints) {
        if (s.halfWidth > 0.0f) {
            scale.push_back(s.halfWidth);
        }
    }
    if (scale.empty()) {
        for (int i = 0; i < n; ++i) {
            scale.push_back(arc[static_cast<std::size_t>(i) + 1] - arc[static_cast<std::size_t>(i)]);
        }
        RT_LOGF(RT_TAG_A11Y, "course map: no corridor widths, spacing from the route's segments\n");
    }
    std::nth_element(scale.begin(), scale.begin() + scale.size() / 2, scale.end());
    const float spacing = scale[scale.size() / 2];
    if (!(spacing > 0.0f) || !(lap > 0.0f)) {
        return;
    }
    const int count = std::max(kMinStations, static_cast<int>(std::lround(lap / spacing)));
    const float step = lap / static_cast<float>(count);  // exact, so the seam closes

    // Where each authored vertex sits along the polyline this walks, kept before the stations
    // replace it: the corner model works in this arc domain, and station i is exactly i steps
    // along it, so one division turns a vertex arc into a fractional station position.
    mVertexArc.assign(arc.begin(), arc.begin() + n);
    mVertexArcStep = step;

    std::vector<Station> resampled;
    resampled.reserve(static_cast<std::size_t>(count));
    int seg = 0;
    for (int k = 0; k < count; ++k) {
        const float target = step * static_cast<float>(k);
        while (seg + 1 < n && arc[static_cast<std::size_t>(seg) + 1] <= target) {
            ++seg;
        }
        const Station& a = mPoints[static_cast<std::size_t>(seg)];
        const Station& b = mPoints[static_cast<std::size_t>(Wrap(seg + 1))];
        const float len = arc[static_cast<std::size_t>(seg) + 1] - arc[static_cast<std::size_t>(seg)];
        const float t = len > 0.0f ? std::clamp((target - arc[static_cast<std::size_t>(seg)]) / len, 0.0f, 1.0f) : 0.0f;
        Station s;
        s.x = a.x + (b.x - a.x) * t;
        s.z = a.z + (b.z - a.z) * t;
        s.y = a.y + (b.y - a.y) * t;
        s.halfWidth = a.halfWidth + (b.halfWidth - a.halfWidth) * t;
        resampled.push_back(s);
    }
    RT_LOGF(RT_TAG_A11Y, "course map: resampled %d route points to %d stations every %.0f units\n",
            n, count, static_cast<double>(step));
    mPoints = std::move(resampled);
}

// The positions arrive already smoothed - RouteGraph::Build does it once, on the way in, so the
// stations and the lateral offset cannot drift onto two different lines.
bool CourseMap::BuildRouteStations() {
    if (!mGraph.Loaded()) {
        return false;
    }
    const std::vector<int>& lap = mGraph.Lap();
    mLapVertices = lap;  // the corner model reads these points' AUTHORED positions
    mPoints.reserve(lap.size());
    for (int index : lap) {
        const RoutePoint& point = mGraph.Point(index);
        Station station;
        station.x = point.x;
        station.z = point.z;
        station.y = point.y;
        station.halfWidth = point.range * kCorridorPerRange;
        mPoints.push_back(station);
    }
    return Loaded();
}

// Only for ordering, and only when the route never closed a lap. The midpoint of a checkpoint quad
// is not the middle of the road - the quad is a lap-validation volume and the game discards its
// width after parsing - so these stations carry progress and corner shape but never position. The
// lateral query stays on the route.
void CourseMap::BuildCheckpointStations(const std::vector<Checkpoint>& checkpoints) {
    RT_LOGF(RT_TAG_A11Y, "course map: no route lap, ordering from %d checkpoints\n",
            static_cast<int>(checkpoints.size()));
    mPoints.clear();  // never mix fallback midpoints with partial route stations
    mLapVertices.clear();  // and no authored vertices, so this map has no corners
    mPoints.reserve(checkpoints.size());
    for (const Checkpoint& cp : checkpoints) {
        Station station;
        station.x = (cp.leftX + cp.rightX) * 0.5f;
        station.z = (cp.leftZ + cp.rightZ) * 0.5f;
        station.halfWidth = Hypot2(cp.rightX - cp.leftX, cp.rightZ - cp.leftZ) * 0.5f;
        mPoints.push_back(station);
    }
}

void CourseMap::BuildDerived() {
    const int n = StationCount();
    mArc.resize(n);

    // Arc length runs from station zero and closes the loop, so the lap length is the distance back
    // to the start rather than the distance to the last station.
    mArc[0] = 0.0f;
    float running = 0.0f;
    for (int i = 1; i < n; ++i) {
        float ax, az, bx, bz;
        Centre(i - 1, ax, az);
        Centre(i, bx, bz);
        running += Hypot2(bx - ax, bz - az);
        mArc[i] = running;
    }
    float lastX, lastZ, firstX, firstZ;
    Centre(n - 1, lastX, lastZ);
    Centre(0, firstX, firstZ);
    mLapLength = running + Hypot2(firstX - lastX, firstZ - lastZ);
    mMeanSpacing = mLapLength / static_cast<float>(n);

    std::vector<float> widths;
    widths.reserve(n);
    for (const Station& s : mPoints) {
        if (s.halfWidth > 0.0f) {
            widths.push_back(s.halfWidth);
        }
    }
    if (!widths.empty()) {
        std::nth_element(widths.begin(), widths.begin() + widths.size() / 2, widths.end());
        mMedianHalfWidth = widths[widths.size() / 2];
    }
}

// Settles which perpendicular is "right", and maps each of the game's checkpoint indices onto the
// nearest station so the game's own progress can index this map.
void CourseMap::BuildCheckpointMap(const std::vector<Checkpoint>& checkpoints) {
    mStationForCheckpoint.assign(checkpoints.size(), -1);
    if (checkpoints.empty()) {
        return;
    }

    // Each checkpoint is bound from the previous one's station, so the mapping walks the lap in
    // order instead of letting a crossover snap a midpoint onto the far branch. Only the first is
    // a global search, having no predecessor to start from.
    int hint = -1;
    for (std::size_t c = 0; c < checkpoints.size(); ++c) {
        const Checkpoint& cp = checkpoints[c];
        const float midX = (cp.leftX + cp.rightX) * 0.5f;
        const float midZ = (cp.leftZ + cp.rightZ) * 0.5f;
        hint = NearestStation(midX, midZ, hint);
        mStationForCheckpoint[c] = hint;
    }

    // A checkpoint states its own left and right points, which is the only place in the data that
    // says which side is which. Settled by a vote across every checkpoint rather than trusting
    // the first: one degenerate or skewed quad deciding the sign silently would mirror every
    // panned cue and every spoken corner side on the course. A checkpoint only votes when its
    // left-to-right vector is at least half-aligned with the perpendicular being judged, and the
    // tally is logged so a near-tie is visible instead of silent.
    int forVotes = 0;
    int againstVotes = 0;
    for (std::size_t c = 0; c < checkpoints.size(); ++c) {
        const Checkpoint& cp = checkpoints[c];
        float fx, fz;
        Forward(mStationForCheckpoint[c], fx, fz);
        const float toRightX = cp.rightX - cp.leftX;
        const float toRightZ = cp.rightZ - cp.leftZ;
        const float cross = fz * toRightX - fx * toRightZ;
        const float span = Hypot2(toRightX, toRightZ);
        if (span > 0.0f && std::fabs(cross) > span * 0.5f) {
            (cross > 0.0f ? forVotes : againstVotes) += 1;
        }
    }
    mRightPerpSign = forVotes >= againstVotes ? 1.0f : -1.0f;
    // All three counters, because "+3 of 40" cannot distinguish a coin flip from mass abstention
    // - and a mirrored sign here mirrors every panned cue and every spoken corner on the course.
    const int total = static_cast<int>(checkpoints.size());
    RT_LOGF(RT_TAG_A11Y,
            "course map: right-hand vote: %d for, %d against, %d abstained of %d -> sign %+.0f\n",
            forVotes, againstVotes, total - forVotes - againstVotes, total,
            static_cast<double>(mRightPerpSign));
    if (forVotes == againstVotes || (forVotes + againstVotes) * 2 < total) {
        RT_LOGF(RT_TAG_A11Y, "course map: WARNING - the right-hand vote is weak on this course\n");
    }

    // How coarse the checkpoint index is in stations, so ArcOfPosition searches just wide enough
    // to cover the ground the kart can be ahead of its mapped station. Bounded: one mis-mapped
    // checkpoint (a branch, a crossover) can make the widest gap span most of the lap, and a
    // window that wide turns ArcOfPosition back into the global nearest-point search the
    // checkpoint hint exists to avoid. An eighth of a lap bounds it - no real checkpoint spacing
    // is coarser - and an outlier is logged instead of obeyed.
    const int n = StationCount();
    int widest = 1;
    for (std::size_t c = 0; c < checkpoints.size() && n > 0; ++c) {
        const int from = mStationForCheckpoint[c];
        const int to = mStationForCheckpoint[(c + 1) % checkpoints.size()];
        const int gap = ((to - from) % n + n) % n;
        widest = std::max(widest, gap);
    }
    const int bound = std::max(2, n / 8);
    if (widest + 1 > bound) {
        RT_LOGF(RT_TAG_A11Y, "course map: checkpoint gap of %d stations clamped to %d\n", widest,
                bound);
    }
    mHintWindow = std::min(widest + 1, bound);
}

float CourseMap::ArcForward(int from, int to) const {
    if (!Loaded()) {
        return 0.0f;
    }
    float d = mArc[Wrap(to)] - mArc[Wrap(from)];
    if (d < 0.0f) {
        d += mLapLength;
    }
    return d;
}

float CourseMap::ArcSigned(int from, int to) const {
    if (!Loaded()) {
        return 0.0f;
    }
    float d = ArcForward(from, to);
    if (d > mLapLength * 0.5f) {
        d -= mLapLength;
    }
    return d;
}

float CourseMap::WrapForward(float d) const {
    if (!Loaded() || mLapLength <= 0.0f) {
        return 0.0f;
    }
    d = std::fmod(d, mLapLength);
    return d < 0.0f ? d + mLapLength : d;
}

float CourseMap::ArcForwardTo(float fromArc, int toStation) const {
    return WrapForward(ArcAt(toStation) - fromArc);
}

float CourseMap::ArcForwardFrom(int fromStation, float toArc) const {
    return WrapForward(toArc - ArcAt(fromStation));
}

float CourseMap::ArcSignedTo(float fromArc, int toStation) const {
    float d = ArcForwardTo(fromArc, toStation);
    if (d > mLapLength * 0.5f) {
        d -= mLapLength;
    }
    return d;
}

bool CourseMap::SegmentForArc(float arc, int& station, float& t) const {
    if (!Loaded() || mLapLength <= 0.0f) {
        return false;
    }
    arc = std::fmod(arc, mLapLength);
    if (arc < 0.0f) {
        arc += mLapLength;
    }
    const int n = StationCount();
    // mArc accumulates chord lengths from zero, so it is sorted: the segment is the last station
    // whose arc is not past this one. A linear scan ran on every arc query and there are several
    // per frame per cue.
    const auto after = std::upper_bound(mArc.begin(), mArc.end(), arc);
    station = std::clamp(static_cast<int>(after - mArc.begin()) - 1, 0, n - 1);
    const float start = mArc[station];
    const float length = (station + 1 < n) ? (mArc[station + 1] - start) : (mLapLength - start);
    t = length > 0.0f ? std::clamp((arc - start) / length, 0.0f, 1.0f) : 0.0f;
    return true;
}

// The blend the arc queries share. Two unit forwards that nearly cancel - the two sides of a
// hairpin taken in two stations - lerp to a vector whose direction is noise, and normalising it
// hands back a confident wrong heading; zero is the honest answer and every caller already treats
// a null forward as "no direction here".
void CourseMap::ForwardInSegment(int station, float t, float& x, float& z) const {
    x = 0.0f;
    z = 0.0f;
    float ax, az, bx, bz;
    Forward(station, ax, az);
    Forward(station + 1, bx, bz);
    const float mixX = ax + (bx - ax) * t;
    const float mixZ = az + (bz - az) * t;
    const float len = Hypot2(mixX, mixZ);
    if (len < kForwardBlendMinLength) {
        return;
    }
    x = mixX / len;
    z = mixZ / len;
}

bool CourseMap::SegmentAtArc(float arc, int& station, float& t) const {
    return SegmentForArc(arc, station, t);
}

void CourseMap::ForwardAtArc(float arc, float& x, float& z) const {
    x = 0.0f;
    z = 0.0f;
    int station = 0;
    float t = 0.0f;
    if (!SegmentForArc(arc, station, t)) {
        return;
    }
    ForwardInSegment(station, t, x, z);
}

bool CourseMap::PointAtArc(float arc, float& x, float& z) const {
    x = 0.0f;
    z = 0.0f;
    int station = 0;
    float t = 0.0f;
    if (!SegmentForArc(arc, station, t)) {
        return false;
    }
    float ax, az, bx, bz;
    Centre(station, ax, az);
    Centre(station + 1, bx, bz);
    x = ax + (bx - ax) * t;
    z = az + (bz - az) * t;
    return true;
}

float CourseMap::ArcOfPosition(float x, float z, int hintStation) const {
    if (!Loaded()) {
        return 0.0f;
    }
    float bestArc = ArcAt(hintStation);
    float bestDistSq = -1.0f;
    for (int step = -mHintWindow; step < mHintWindow; ++step) {
        const int i = Wrap(hintStation + step);
        float ax, az, bx, bz;
        Centre(i, ax, az);
        Centre(i + 1, bx, bz);
        const float ux = bx - ax, uz = bz - az;
        const float lenSq = ux * ux + uz * uz;
        if (lenSq <= 0.0f) {
            continue;
        }
        const float t = std::clamp(((x - ax) * ux + (z - az) * uz) / lenSq, 0.0f, 1.0f);
        const float dx = x - (ax + ux * t);
        const float dz = z - (az + uz * t);
        const float distSq = dx * dx + dz * dz;
        if (bestDistSq < 0.0f || distSq < bestDistSq) {
            bestDistSq = distSq;
            float arc = ArcAt(i) + std::sqrt(lenSq) * t;
            if (arc >= mLapLength) {
                arc -= mLapLength;  // the closing segment runs past the lap total
            }
            bestArc = arc;
        }
    }
    return bestArc;
}

int CourseMap::NearestStation(float x, float z, int hint) const {
    const int n = StationCount();
    if (n <= 0) {
        return 0;
    }

    int best = 0;
    float bestDist = -1.0f;
    const float window = mLapLength * kSearchArcFraction;

    for (int i = 0; i < n; ++i) {
        if (hint >= 0) {
            const float ahead = ArcForward(hint, i);
            if (ahead > window && (mLapLength - ahead) > window) {
                continue;
            }
        }
        float cx, cz;
        Centre(i, cx, cz);
        const float dx = x - cx, dz = z - cz;
        const float d = dx * dx + dz * dz;
        if (bestDist < 0.0f || d < bestDist) {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

bool CourseMap::RoadOffsetAtArc(float arc, float x, float y, float z, float& out,
                                float* closestXOut, float* closestZOut,
                                float* halfWidthOut) const {
    out = 0.0f;
    if (!mRouteBased) {
        // No lap to measure against: the graph-wide search is all there is.
        return mGraph.LateralOffset(x, y, z, mRightPerpSign, out, closestXOut, closestZOut,
                                    halfWidthOut);
    }
    // Resolved once and reused for the point, the corridor and the perpendicular: going back
    // through PointAtArc and RightVectorAtArc re-ran the segment search twice more per query.
    int station = 0;
    float t = 0.0f;
    if (!SegmentForArc(arc, station, t)) {
        return false;
    }
    float ax, az, bx, bz;
    Centre(station, ax, az);
    Centre(station + 1, bx, bz);
    const float lineX = ax + (bx - ax) * t;
    const float lineZ = az + (bz - az) * t;
    // Not `near`/`far`: both are macros in the Windows headers this translation unit pulls in.
    const float widthHere = HalfWidth(station);
    const float widthNext = HalfWidth(station + 1);
    const float halfWidth = widthHere + (widthNext - widthHere) * t;
    if (!(halfWidth > 0.0f)) {
        return false;
    }
    float forwardX = 0.0f, forwardZ = 0.0f;
    ForwardInSegment(station, t, forwardX, forwardZ);
    // No forward here means no "across the line" either. Falling through returned a confident
    // offset of exactly zero - "you are perfectly centred" - from a point with no direction at all.
    if (forwardX == 0.0f && forwardZ == 0.0f) {
        return false;
    }
    const float rightX = forwardZ * mRightPerpSign;
    const float rightZ = -forwardX * mRightPerpSign;
    if (closestXOut != nullptr) {
        *closestXOut = lineX;
    }
    if (closestZOut != nullptr) {
        *closestZOut = lineZ;
    }
    if (halfWidthOut != nullptr) {
        *halfWidthOut = halfWidth;
    }
    out = ((x - lineX) * rightX + (z - lineZ) * rightZ) / halfWidth;
    return true;
}

bool CourseMap::ApplyRoadShift(const std::vector<float>& shifts) {
    const int n = StationCount();
    if (mRoadShifted || !mRouteBased || static_cast<int>(shifts.size()) != n) {
        return false;
    }
    mRoadShifted = true;  // one attempt per course, successful or not

    // Applied exactly as solved: the racing line is already the smoothest line its bands allow,
    // and any averaging here would only bend it back towards the authored route.
    int moved = 0;
    for (int i = 0; i < n; ++i) {
        if (shifts[i] != 0.0f) {
            ++moved;
        }
    }
    if (moved == 0) {
        EdgeMap::ConfirmShift(false);
        return false;  // the whole line was already on the road
    }

    // Every right vector first: it is built from the neighbouring stations, so reading it while the
    // stations are moving would measure each one against a half-corrected line.
    std::vector<float> rightX(static_cast<std::size_t>(n), 0.0f);
    std::vector<float> rightZ(static_cast<std::size_t>(n), 0.0f);
    for (int i = 0; i < n; ++i) {
        RightVector(i, rightX[i], rightZ[i]);
    }

    float worst = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float dx = rightX[i] * shifts[i];
        const float dz = rightZ[i] * shifts[i];
        mPoints[i].x += dx;
        mPoints[i].z += dz;
        worst = std::max(worst, std::fabs(shifts[i]));
    }

    // Arcs descend from the positions, so they are rebuilt and the corners' landmarks re-read
    // from them. The corners themselves, the vertices they came from, the right-hand sign and
    // the checkpoint mapping are NOT: the corners describe the road, and the sign is the mod's
    // one direction convention. (A racing line that crosses the road between two corners bends
    // in an S there; counting that S as corners and reverting the whole line on it kept every
    // course with such a crossing on the CPU's route until 2026-09-03.)
    // The corner list is the same list, so the generation stays: a shift landing on a lap
    // boundary must not un-say what was said about the corners ahead.
    BuildDerived();
    RefreshCurveArcs();

    // The placement stuck: the edge map rebases its distances onto the line the stations now have.
    EdgeMap::ConfirmShift(true);
    RT_LOGF(RT_TAG_A11Y,
            "course map: racing line placed, %d of %d stations moved, largest %.0f, "
            "lap %.0f\n",
            moved, n, static_cast<double>(worst), static_cast<double>(mLapLength));
    return true;
}

}  // namespace a11y::race
