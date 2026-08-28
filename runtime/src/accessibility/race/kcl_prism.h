#ifndef MKW_ACCESSIBILITY_RACE_KCL_PRISM_H
#define MKW_ACCESSIBILITY_RACE_KCL_PRISM_H

#include <cstdint>

namespace a11y::race {

struct KclMesh;

// kcl_lib.py:120 - a surface steeper than about 87 degrees is a wall to a vertical ray. Shared,
// because the object overlay uses the same tolerance to decide whether an object's own vertical
// still points up in the world (docs/kcl-runtime.md §5).
inline constexpr float kKclMinFloorNormalY = 0.05f;

// One reconstructed prism: the plane a vertical ray hits, plus the XZ projection that says whether
// the ray hits it at all.
struct KclFace {
    float nx = 0.0f, ny = 0.0f, nz = 0.0f, d = 0.0f;
    float ax = 0.0f, az = 0.0f, bx = 0.0f, bz = 0.0f, cx = 0.0f, cz = 0.0f;
    std::uint8_t baseType = 0;
};

// Rebuilds prism `index` (1-based, as the octree lists it) of `mesh` as a floor triangle. False
// when the prism is degenerate, is a wall or a pure trigger, is too steep to be a floor, or reads
// out of range - all of which mean "not ground under this point". The mesh is a parameter because
// an object's KCL has the same layout and the same meaning (docs/kcl-runtime.md §5).
bool KclDecodePrism(const KclMesh& mesh, std::uint32_t index, KclFace& face);

// Whether the face's XZ projection covers the point.
bool KclCoversXZ(const KclFace& face, float x, float z);

// The face's height at (x, z). Only meaningful when KclCoversXZ agrees.
inline float KclHeightAt(const KclFace& face, float x, float z) {
    return -(face.nx * x + face.nz * z + face.d) / face.ny;
}

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_KCL_PRISM_H
