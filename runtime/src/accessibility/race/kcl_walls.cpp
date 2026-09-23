#include "accessibility/race/kcl_walls.h"

#include <cmath>
#include <cstdint>

#include "accessibility/race/kcl_column.h"
#include "accessibility/race/kcl_mesh.h"
#include "accessibility/race/kcl_prism.h"

namespace a11y::race {
namespace {

constexpr std::uint32_t WallTypeBit(std::uint8_t type) { return 1u << (type & kKclBaseTypeMask); }

// The walls a kart bumps into: wall, invisible wall, wall 3, player-only wall, weak wall and
// invisible wall 2. Not the item-only wall (0x0E), the half-pipe's own wall (0x1C) or the special
// wall (0x1E) that rims Mushroom Gorge's caps, which the kart goes over into the void
// (docs/kcl-runtime.md; the edge sweep already calls that a fall).
constexpr std::uint32_t kKartWallTypes = WallTypeBit(0x0C) | WallTypeBit(0x0D) | WallTypeBit(0x0F) |
                                         WallTypeBit(0x14) | WallTypeBit(0x19) | WallTypeBit(0x1F);

// Two heights up the kart's side: above a kerb it would ride over, below a roof it would pass
// under. A wall that crosses either is a wall the body meets.
constexpr float kWallProbeHeights[] = {50.0f, 120.0f};

// Safety bound, as kcl_column.cpp's: no leaf holds this many prisms.
constexpr int kWallLeafPrisms = 1024;
constexpr int kWallLeaves = 4;
constexpr float kWallParallelEpsilon = 1e-6f;

bool IsKartWall(const KclMesh& m, std::uint16_t index) {
    std::uint16_t attribute = 0;
    return KclTryPrismAttribute(m, index, attribute) &&
           (kKartWallTypes & WallTypeBit(static_cast<std::uint8_t>(attribute & kKclBaseTypeMask))) != 0;
}

// Moller-Trumbore on the segment p + d * t, t in [0, 1].
bool SegmentHits(const KclTriangle& tri, const float p[3], const float d[3], float& t) {
    const float* a = tri.v[0];
    float e1[3], e2[3], s[3], h[3], q[3];
    for (int k = 0; k < 3; ++k) {
        e1[k] = tri.v[1][k] - a[k];
        e2[k] = tri.v[2][k] - a[k];
        s[k] = p[k] - a[k];
    }
    h[0] = d[1] * e2[2] - d[2] * e2[1];
    h[1] = d[2] * e2[0] - d[0] * e2[2];
    h[2] = d[0] * e2[1] - d[1] * e2[0];
    const float det = e1[0] * h[0] + e1[1] * h[1] + e1[2] * h[2];
    if (std::fabs(det) < kWallParallelEpsilon) {
        return false;
    }
    const float inv = 1.0f / det;
    const float u = inv * (s[0] * h[0] + s[1] * h[1] + s[2] * h[2]);
    if (u < 0.0f || u > 1.0f) {
        return false;
    }
    q[0] = s[1] * e1[2] - s[2] * e1[1];
    q[1] = s[2] * e1[0] - s[0] * e1[2];
    q[2] = s[0] * e1[1] - s[1] * e1[0];
    const float v = inv * (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]);
    if (v < 0.0f || u + v > 1.0f) {
        return false;
    }
    t = inv * (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]);
    return t >= 0.0f && t <= 1.0f;
}

}  // namespace

bool KclWallAcross(float x0, float z0, float x1, float z1, float floorY, float& t) {
    const KclMesh& m = KclMeshData();
    if (!m.ready) {
        return false;
    }
    bool hit = false;
    t = 1.0f;
    for (float up : kWallProbeHeights) {
        const float y = floorY + up;
        // A prism is listed in every leaf its box overlaps, so the leaves holding the step's two
        // ends hold every wall that can cross a step shorter than a leaf.
        std::uint32_t leaves[kWallLeaves] = {};
        int count = 0;
        for (std::uint32_t list : {KclLeafListAt(m, x0, y, z0), KclLeafListAt(m, x1, y, z1)}) {
            bool seen = list == 0;
            for (int k = 0; k < count && !seen; ++k) seen = leaves[k] == list;
            if (!seen && count < kWallLeaves) leaves[count++] = list;
        }
        const float p[3] = {x0, y, z0};
        const float d[3] = {x1 - x0, 0.0f, z1 - z0};
        for (int k = 0; k < count; ++k) {
            std::uint16_t indices[kWallLeafPrisms];
            const int n = KclLeafPrisms(leaves[k], indices, kWallLeafPrisms);
            for (int i = 0; i < n; ++i) {
                KclTriangle tri;
                float at = 0.0f;
                if (IsKartWall(m, indices[i]) && KclDecodeTriangle(m, indices[i], tri) &&
                    SegmentHits(tri, p, d, at) && at < t) {
                    t = at;
                    hit = true;
                }
            }
        }
    }
    return hit;
}

}  // namespace a11y::race
