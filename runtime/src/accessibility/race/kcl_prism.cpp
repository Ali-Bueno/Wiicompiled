#include "accessibility/race/kcl_prism.h"

#include <cmath>

#include "accessibility/race/kcl_mesh.h"

namespace a11y::race {
namespace {

// Prism fields (docs/kcl-runtime.md §2). Two u16 fields share each word after the height.
constexpr std::uint32_t kPrismHeight = 0x00;
constexpr std::uint32_t kPrismPosFnrmWord = 0x04;    // posIdx, then fnrmIdx
constexpr std::uint32_t kPrismEnrm12Word = 0x08;     // enrm1, then enrm2
constexpr std::uint32_t kPrismEnrm3AttrWord = 0x0C;  // enrm3, then attribute

// kcl_lib.py:93 - edge normals coplanar with the face leave the prism no area.
constexpr float kDegeneratePrism = 1e-9f;

void KclCross(float ax, float ay, float az, float bx, float by, float bz, float& x, float& y,
              float& z) {
    x = ay * bz - az * by;
    y = az * bx - ax * bz;
    z = ax * by - ay * bx;
}

bool KclReadVec(std::uint32_t base, std::uint32_t index, float& x, float& y, float& z) {
    return KclTryVec3(base + index * kKclVecStride, x, y, z);
}

}  // namespace

// kcl_lib.py:78-98: a prism stores one vertex plus the face and edge normals, and the other two
// vertices fall out of cross(edgeNormal, faceNormal) scaled by the prism height.
bool KclDecodePrism(const KclMesh& m, std::uint32_t index, KclFace& face) {
    if (index == 0 || index > m.prismCount) {
        return false;
    }
    const std::uint32_t prism = m.prismData + index * kKclPrismStride;
    float height = 0.0f;
    std::uint16_t posIdx = 0, fnrmIdx = 0, e1 = 0, e2 = 0, e3 = 0, attribute = 0;
    if (!KclTryFloat(prism + kPrismHeight, height) ||
        !KclTryU16(prism + kPrismPosFnrmWord, /*highHalf=*/true, posIdx) ||
        !KclTryU16(prism + kPrismPosFnrmWord, /*highHalf=*/false, fnrmIdx) ||
        !KclTryU16(prism + kPrismEnrm12Word, /*highHalf=*/true, e1) ||
        !KclTryU16(prism + kPrismEnrm12Word, /*highHalf=*/false, e2) ||
        !KclTryU16(prism + kPrismEnrm3AttrWord, /*highHalf=*/true, e3) ||
        !KclTryU16(prism + kPrismEnrm3AttrWord, /*highHalf=*/false, attribute) ||
        !(height > 0.0f)) {
        return false;
    }
    const std::uint8_t baseType = static_cast<std::uint8_t>(attribute & kKclBaseTypeMask);
    if (!KclBaseTypeCanBeFloor(baseType)) {
        return false;
    }
    if (posIdx >= m.posCount || fnrmIdx >= m.nrmCount || e1 >= m.nrmCount || e2 >= m.nrmCount ||
        e3 >= m.nrmCount) {
        return false;
    }
    float px, py, pz, dx, dy, dz, n1x, n1y, n1z, n2x, n2y, n2z, n3x, n3y, n3z;
    if (!KclReadVec(m.posData, posIdx, px, py, pz) ||
        !KclReadVec(m.nrmData, fnrmIdx, dx, dy, dz) || !KclReadVec(m.nrmData, e1, n1x, n1y, n1z) ||
        !KclReadVec(m.nrmData, e2, n2x, n2y, n2z) || !KclReadVec(m.nrmData, e3, n3x, n3y, n3z)) {
        return false;
    }
    float cax, cay, caz, cbx, cby, cbz;
    KclCross(n1x, n1y, n1z, dx, dy, dz, cax, cay, caz);
    KclCross(n2x, n2y, n2z, dx, dy, dz, cbx, cby, cbz);
    const float da = cax * n3x + cay * n3y + caz * n3z;
    const float db = cbx * n3x + cby * n3y + cbz * n3z;
    if (std::fabs(da) < kDegeneratePrism || std::fabs(db) < kDegeneratePrism) {
        return false;
    }
    const float ta = height / da;
    const float tb = height / db;
    const float v1x = px + cbx * tb, v1y = py + cby * tb, v1z = pz + cbz * tb;
    const float v2x = px + cax * ta, v2y = py + cay * ta, v2z = pz + caz * ta;

    // The plane comes from the reconstructed triangle rather than the stored face normal, so the
    // ray and the coverage test agree by construction (kcl_lib.py:135-149).
    float nx, ny, nz;
    KclCross(v1x - px, v1y - py, v1z - pz, v2x - px, v2y - py, v2z - pz, nx, ny, nz);
    const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (!(length > 0.0f)) {
        return false;
    }
    nx /= length;
    ny /= length;
    nz /= length;
    if (ny < 0.0f) {
        nx = -nx;
        ny = -ny;
        nz = -nz;
    }
    if (ny < kKclMinFloorNormalY) {
        return false;
    }
    face.nx = nx;
    face.ny = ny;
    face.nz = nz;
    face.d = -(nx * px + ny * py + nz * pz);
    face.ax = px;
    face.az = pz;
    face.bx = v1x;
    face.bz = v1z;
    face.cx = v2x;
    face.cz = v2z;
    face.baseType = baseType;
    return true;
}

// kcl_lib.py:165-170 - the winding is not fixed, so either sign is accepted as long as all three
// edges agree.
bool KclCoversXZ(const KclFace& f, float x, float z) {
    const float d1 = (f.bx - f.ax) * (z - f.az) - (f.bz - f.az) * (x - f.ax);
    const float d2 = (f.cx - f.bx) * (z - f.bz) - (f.cz - f.bz) * (x - f.bx);
    const float d3 = (f.ax - f.cx) * (z - f.cz) - (f.az - f.cz) * (x - f.cx);
    const bool negative = d1 < 0.0f || d2 < 0.0f || d3 < 0.0f;
    const bool positive = d1 > 0.0f || d2 > 0.0f || d3 > 0.0f;
    return !(negative && positive);
}

}  // namespace a11y::race
