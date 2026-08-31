#include "accessibility/race/course_map.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "accessibility/a11y_log.h"
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
    mTurn.clear();
    mStationForCheckpoint.clear();
    mCurves.clear();
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
    if (!routeLap) {
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
    // Curvature signs read the right vector, which BuildCheckpointMap settles, so this has to run
    // after it.
    for (int i = 0; i < StationCount(); ++i) {
        mTurn[i] = SignedTurnAt(i);
    }
    BuildCurves();
    mRouteBased = routeLap && lapSane;
    return mRouteBased;
}

// The positions arrive already smoothed - RouteGraph::Build does it once, on the way in, so the
// stations and the lateral offset cannot drift onto two different lines.
bool CourseMap::BuildRouteStations() {
    if (!mGraph.Loaded()) {
        return false;
    }
    const std::vector<int>& lap = mGraph.Lap();
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
    mTurn.assign(n, 0.0f);

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

    for (std::size_t c = 0; c < checkpoints.size(); ++c) {
        const Checkpoint& cp = checkpoints[c];
        const float midX = (cp.leftX + cp.rightX) * 0.5f;
        const float midZ = (cp.leftZ + cp.rightZ) * 0.5f;
        mStationForCheckpoint[c] = NearestStation(midX, midZ, /*hint=*/-1);
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

// Curvature at a station, signed positive to the right, in radians of heading change per unit of
// travel - the reciprocal of the corner radius.
float CourseMap::SignedTurnAt(int i) const {
    float px, pz, cx, cz, nx, nz;
    Centre(i - 1, px, pz);
    Centre(i, cx, cz);
    Centre(i + 1, nx, nz);

    float ax = cx - px, az = cz - pz;
    float bx = nx - cx, bz = nz - cz;
    const float aLen = Hypot2(ax, az);
    const float bLen = Hypot2(bx, bz);
    if (aLen <= 0.0f || bLen <= 0.0f) {
        return 0.0f;
    }
    ax /= aLen;
    az /= aLen;
    bx /= bLen;
    bz /= bLen;

    const float dot = std::clamp(ax * bx + az * bz, -1.0f, 1.0f);
    const float angle = std::acos(dot);

    // Signed against the station's own right vector, so "right" stays the one convention the whole
    // mod shares.
    float rx, rz;
    RightVector(i, rx, rz);
    // A station whose neighbours coincide - a hairpin taken in two stations - has no forward, so
    // Forward leaves a null vector and its perpendicular is null too. Zero is the honest answer:
    // falling through would grade every such corner as a right-hander on `sideChange >= 0`.
    if (Hypot2(rx, rz) <= 0.0f) {
        return 0.0f;
    }
    const float sideChange = (bx * rx + bz * rz) - (ax * rx + az * rz);

    const float span = (aLen + bLen) * 0.5f;
    const float curvature = span > 0.0f ? angle / span : 0.0f;
    return sideChange >= 0.0f ? curvature : -curvature;
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

float CourseMap::ArcSignedTo(float fromArc, int toStation) const {
    if (!Loaded() || mLapLength <= 0.0f) {
        return 0.0f;
    }
    float d = ArcAt(toStation) - fromArc;
    while (d < 0.0f) {
        d += mLapLength;
    }
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
    for (int i = 0; i < n; ++i) {
        const float start = mArc[i];
        const float length = (i + 1 < n) ? (mArc[i + 1] - start) : (mLapLength - start);
        if (arc > start + length) {
            continue;  // the scan runs in arc order, so the first fit is the segment
        }
        station = i;
        t = length > 0.0f ? std::clamp((arc - start) / length, 0.0f, 1.0f) : 0.0f;
        return true;
    }
    station = n - 1;
    t = 1.0f;
    return true;
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
    float ax, az, bx, bz;
    Forward(station, ax, az);
    Forward(station + 1, bx, bz);
    x = ax + (bx - ax) * t;
    z = az + (bz - az) * t;
    const float len = Hypot2(x, z);
    if (len > 0.0f) {
        x /= len;
        z /= len;
    }
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
    int station = 0;
    float t = 0.0f;
    float lineX = 0.0f, lineZ = 0.0f;
    if (!SegmentForArc(arc, station, t) || !PointAtArc(arc, lineX, lineZ)) {
        return false;
    }
    // Not `near`/`far`: both are macros in the Windows headers this translation unit pulls in.
    const float widthHere = HalfWidth(station);
    const float widthNext = HalfWidth(station + 1);
    const float halfWidth = widthHere + (widthNext - widthHere) * t;
    if (!(halfWidth > 0.0f)) {
        return false;
    }
    float rightX = 0.0f, rightZ = 0.0f;
    RightVectorAtArc(arc, rightX, rightZ);
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

    // Applied exactly as measured. This was a 3-tap average of the neighbouring shifts, because on
    // 2026-08-31 an undiluted 1,000-unit correction at one station beside an untouched neighbour
    // added a fourth curve to the map - `entry=4 apex=4 exit=4 right arc=0`, spoken as "gentle
    // right" where no right turn fits. EdgeMap::CentreLine now bounds the STEP between neighbouring
    // shifts rather than the shifts themselves, so the profile already arrives continuous and the
    // average has nothing left to smooth. It does still have something to break: it cut a required
    // move to a third of itself, and that move is the whole point of the repair.
    int moved = 0;
    for (int i = 0; i < n; ++i) {
        if (shifts[i] != 0.0f) {
            ++moved;
        }
    }
    if (moved == 0) {
        return false;  // the whole line was already on the road
    }

    // Every right vector first: it is built from the neighbouring stations, so reading it while the
    // stations are moving would measure each one against a half-corrected line.
    std::vector<float> rightX(static_cast<std::size_t>(n), 0.0f);
    std::vector<float> rightZ(static_cast<std::size_t>(n), 0.0f);
    for (int i = 0; i < n; ++i) {
        RightVector(i, rightX[i], rightZ[i]);
    }

    // Kept so the whole repair can be undone if it turns out to have changed the shape of the
    // course rather than the placement of the line - see the corner check below.
    const std::vector<Station> before = mPoints;
    const std::size_t curvesBefore = mCurves.size();

    float worst = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float dx = rightX[i] * shifts[i];
        const float dz = rightZ[i] * shifts[i];
        mPoints[i].x += dx;
        mPoints[i].z += dz;
        worst = std::max(worst, std::fabs(shifts[i]));
    }

    // Arc, curvature and corners all descend from the positions, so they are rebuilt. The
    // right-hand sign and the checkpoint mapping are NOT: the sign is the mod's one direction
    // convention and re-voting it on a moved line could flip every panned cue mid-race.
    BuildDerived();
    for (int i = 0; i < n; ++i) {
        mTurn[i] = SignedTurnAt(i);
    }
    BuildCurves();

    // The repair moves the line WITHIN the road; it must never change what the course is. A repair
    // that adds a corner has bent the line instead of placing it, and the player is then told to
    // turn where the track does not - which is exactly what happened on 2026-08-31, when an
    // uncapped correction produced a zero-length "gentle right" at the shifted station and cost a
    // whole play-test. The step cap in EdgeMap::CentreLine should make this unreachable;
    // this is the check that says so out loud rather than trusting the arithmetic.
    if (mCurves.size() > curvesBefore) {
        const std::size_t curvesAfter = mCurves.size();  // read before the rebuild restores it
        mPoints = before;
        BuildDerived();
        for (int i = 0; i < n; ++i) {
            mTurn[i] = SignedTurnAt(i);
        }
        BuildCurves();
        RT_LOGF(RT_TAG_A11Y,
                "course map: line repair REVERTED - it added %d corner(s) to the course (%d -> %d), "
                "worst shift %.0f\n",
                static_cast<int>(curvesAfter - curvesBefore), static_cast<int>(curvesBefore),
                static_cast<int>(curvesAfter), static_cast<double>(worst));
        return false;
    }

    RT_LOGF(RT_TAG_A11Y,
            "course map: %d of %d stations were off the road, line repaired, worst shift %.0f, "
            "lap %.0f\n",
            moved, n, static_cast<double>(worst), static_cast<double>(mLapLength));
    return true;
}

int CourseMap::StationAhead(int station, float distance) const {
    const int n = StationCount();
    for (int step = 1; step < n; ++step) {
        const int i = Wrap(station + step);
        if (ArcForward(station, i) >= distance) {
            return i;
        }
    }
    return Wrap(station - 1);
}

void CourseMap::PointAhead(int station, float distance, float& x, float& z) const {
    Centre(station, x, z);
    if (!Loaded() || distance <= 0.0f) {
        return;
    }

    const int n = StationCount();
    for (int step = 0; step < n; ++step) {
        const int from = Wrap(station + step);
        const int to = Wrap(station + step + 1);
        const float travelled = ArcForward(station, from);
        const float leg = ArcForward(from, to);
        if (leg <= 0.0f) {
            continue;
        }
        if (travelled + leg < distance) {
            continue;
        }
        // The remaining distance falls inside this leg, so the target sits partway along it.
        const float t = std::clamp((distance - travelled) / leg, 0.0f, 1.0f);
        float ax, az, bx, bz;
        Centre(from, ax, az);
        Centre(to, bx, bz);
        x = ax + (bx - ax) * t;
        z = az + (bz - az) * t;
        return;
    }
}

}  // namespace a11y::race
