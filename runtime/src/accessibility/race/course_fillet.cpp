#include "accessibility/race/course_map.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "accessibility/a11y_log.h"
#include "accessibility/race/edge_map.h"

namespace a11y::race {
namespace {

// Two samples per station along each arc: the resample then interpolates chords no longer than
// half a station, so it follows the arc instead of cutting it.
constexpr int kArcSamplesPerStation = 2;

// Below this the two segments are parallel to float precision and there is no corner to round.
constexpr float kMinCornerCos = 1e-6f;

struct Corner {
    float ax = 0.0f, az = 0.0f;  // unit direction in
    float bx = 0.0f, bz = 0.0f;  // unit direction out
    float angle = 0.0f;          // turn, radians
    float tangent = 0.0f;        // distance from the vertex to each tangent point; 0 = no arc
    float radius = 0.0f;
};

}  // namespace

// The authored route is a polygon: straight between vertices, a whole corner turned in one place.
// Driven by pure pursuit that sounds as a lean pulse per vertex and a counter-lean after it, never
// the steady lean of a bend, and the pulses arrive faster than a player can answer them as speed
// rises. Each corner is replaced by the largest tangent circular arc that fits: no more than half
// of either neighbouring segment (the two corners on a segment share it), and an apex no deeper
// into the corner than the edge-warning fraction of the vertex's own corridor half-width, so the
// line never runs where the edge cue would sound. A vertex without corridor keeps its corner, and
// so does vertex zero: it is the route's start point, which station zero must stay.
//
// The turn angles the corner model grades are still the authored vertices'; this only changes the
// stations, and records where each vertex's apex now sits along the line for that model.
void CourseMap::FilletCorners(float spacing) {
    const int n = StationCount();
    mVertexArc.assign(static_cast<std::size_t>(n), 0.0f);
    if (n < kMinStations || !(spacing > 0.0f)) {
        float running = 0.0f;
        for (int i = 1; i < n; ++i) {
            const Station& a = mPoints[static_cast<std::size_t>(i - 1)];
            const Station& b = mPoints[static_cast<std::size_t>(i)];
            running += std::hypot(b.x - a.x, b.z - a.z);
            mVertexArc[static_cast<std::size_t>(i)] = running;
        }
        return;
    }

    std::vector<float> segLen(static_cast<std::size_t>(n), 0.0f);
    for (int i = 0; i < n; ++i) {
        const Station& a = mPoints[static_cast<std::size_t>(i)];
        const Station& b = mPoints[static_cast<std::size_t>(Wrap(i + 1))];
        segLen[static_cast<std::size_t>(i)] = std::hypot(b.x - a.x, b.z - a.z);
    }

    std::vector<Corner> corners(static_cast<std::size_t>(n));
    int rounded = 0;
    for (int i = 1; i < n; ++i) {
        Corner& c = corners[static_cast<std::size_t>(i)];
        const float inLen = segLen[static_cast<std::size_t>(Wrap(i - 1))];
        const float outLen = segLen[static_cast<std::size_t>(i)];
        const Station& prev = mPoints[static_cast<std::size_t>(Wrap(i - 1))];
        const Station& here = mPoints[static_cast<std::size_t>(i)];
        const Station& next = mPoints[static_cast<std::size_t>(Wrap(i + 1))];
        if (!(inLen > 0.0f) || !(outLen > 0.0f) || !(here.halfWidth > 0.0f)) {
            continue;
        }
        c.ax = (here.x - prev.x) / inLen;
        c.az = (here.z - prev.z) / inLen;
        c.bx = (next.x - here.x) / outLen;
        c.bz = (next.z - here.z) / outLen;
        const float cosTurn = std::clamp(c.ax * c.bx + c.az * c.bz, -1.0f, 1.0f);
        if (1.0f - cosTurn < kMinCornerCos) {
            continue;
        }
        c.angle = std::acos(cosTurn);
        const float half = 0.5f * c.angle;
        const float tanHalf = std::tan(half);
        const float secHalf = 1.0f / std::cos(half);
        if (!(tanHalf > 0.0f) || !std::isfinite(secHalf)) {
            continue;
        }
        const float bySegments = 0.5f * std::min(inLen, outLen) / tanHalf;
        const float byCorridor = here.halfWidth * kEdgeOnsetRealFraction / (secHalf - 1.0f);
        c.radius = std::min(bySegments, byCorridor);
        c.tangent = c.radius * tanHalf;
        if (c.tangent > 0.0f) {
            ++rounded;
        }
    }

    std::vector<Station> dense;
    dense.reserve(static_cast<std::size_t>(n) * static_cast<std::size_t>(kArcSamplesPerStation) * 2);
    float running = 0.0f;
    auto append = [&](const Station& s) {
        if (!dense.empty()) {
            const Station& last = dense.back();
            running += std::hypot(s.x - last.x, s.z - last.z);
        }
        dense.push_back(s);
    };
    // Vertex zero: the start point, unrounded, arc zero.
    append(mPoints[0]);
    for (int i = 1; i < n; ++i) {
        const Corner& c = corners[static_cast<std::size_t>(i)];
        const Station& here = mPoints[static_cast<std::size_t>(i)];
        if (!(c.tangent > 0.0f)) {
            append(here);
            mVertexArc[static_cast<std::size_t>(i)] = running;
            continue;
        }
        const Station& prev = mPoints[static_cast<std::size_t>(Wrap(i - 1))];
        const Station& next = mPoints[static_cast<std::size_t>(Wrap(i + 1))];
        const float inLen = segLen[static_cast<std::size_t>(Wrap(i - 1))];
        const float outLen = segLen[static_cast<std::size_t>(i)];
        // Tangent points, with height and corridor read off the segments they lie on.
        const float tIn = c.tangent / inLen;
        const float tOut = c.tangent / outLen;
        const float inY = here.y + (prev.y - here.y) * tIn;
        const float outY = here.y + (next.y - here.y) * tOut;
        const float inW = here.halfWidth + (prev.halfWidth - here.halfWidth) * tIn;
        const float outW = here.halfWidth + (next.halfWidth - here.halfWidth) * tOut;
        const float startX = here.x - c.ax * c.tangent;
        const float startZ = here.z - c.az * c.tangent;
        // The arc's centre is a radius to the inside of the incoming direction.
        const float turnSign = (c.ax * c.bz - c.az * c.bx) > 0.0f ? 1.0f : -1.0f;
        const float centreX = startX - c.az * c.radius * turnSign;
        const float centreZ = startZ + c.ax * c.radius * turnSign;
        const float arcLen = c.radius * c.angle;
        int samples = static_cast<int>(std::ceil(arcLen / spacing * static_cast<float>(kArcSamplesPerStation)));
        samples = std::max(2, samples + (samples & 1));  // even, so the apex is a sample
        for (int j = 0; j <= samples; ++j) {
            const float f = static_cast<float>(j) / static_cast<float>(samples);
            const float rot = c.angle * f * turnSign;
            const float dx = startX - centreX;
            const float dz = startZ - centreZ;
            const float cs = std::cos(rot), sn = std::sin(rot);
            Station s;
            s.x = centreX + dx * cs - dz * sn;
            s.z = centreZ + dx * sn + dz * cs;
            s.y = f < 0.5f ? inY + (here.y - inY) * (f * 2.0f) : here.y + (outY - here.y) * ((f - 0.5f) * 2.0f);
            s.halfWidth = f < 0.5f ? inW + (here.halfWidth - inW) * (f * 2.0f)
                                   : here.halfWidth + (outW - here.halfWidth) * ((f - 0.5f) * 2.0f);
            append(s);
            if (j * 2 == samples) {
                mVertexArc[static_cast<std::size_t>(i)] = running;
            }
        }
    }
    RT_LOGF(RT_TAG_A11Y, "course map: rounded %d of %d route corners, %d points\n", rounded, n,
            static_cast<int>(dense.size()));
    mPoints = std::move(dense);
}

}  // namespace a11y::race
