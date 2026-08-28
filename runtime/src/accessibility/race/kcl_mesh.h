#ifndef MKW_ACCESSIBILITY_RACE_KCL_MESH_H
#define MKW_ACCESSIBILITY_RACE_KCL_MESH_H

#include <cmath>
#include <cstdint>
#include <cstring>

#include "accessibility/race/kcl_road.h"
#include "memory.h"

namespace a11y::race {

// Silent guest reads, for addresses derived from guest data.
//
// Memory::TryRead32 swallows the AccessViolation but the runtime has already printed a full CPU
// state and stack dump by then (runtime/src/memory.cpp), once per unmapped read. The KCL reader
// follows octree offsets and leaf lists straight out of guest memory, so a stray address is a
// normal outcome, not an error - and a probe that finds nothing must cost nothing. Memory::Contains
// is the silent test; the second call covers the tail of a word that starts inside a region and
// ends past it. Everything the KCL reader reads goes through these, never through Memory or the
// shared guest_read helpers directly.

inline bool KclTryRead32(std::uint32_t addr, std::uint32_t& out) {
    return Memory::Contains(addr) && Memory::Contains(addr + sizeof(std::uint32_t) - 1) &&
           Memory::TryRead32(addr, out);
}

inline bool KclTryPointer(std::uint32_t addr, std::uint32_t& out) {
    return KclTryRead32(addr, out) && out != 0;
}

inline bool KclTryFloat(std::uint32_t addr, float& out) {
    std::uint32_t bits = 0;
    if (!KclTryRead32(addr, bits)) {
        return false;
    }
    std::memcpy(&out, &bits, sizeof(out));
    return std::isfinite(out);
}

inline bool KclTryVec3(std::uint32_t addr, float& x, float& y, float& z) {
    return KclTryFloat(addr, x) && KclTryFloat(addr + sizeof(float), y) &&
           KclTryFloat(addr + 2 * sizeof(float), z);
}

// A halfword, addressed by the aligned word containing it. After the swap to host order the
// halfword at the word's own address is the high one.
inline bool KclTryU16(std::uint32_t wordAddr, bool highHalf, std::uint16_t& out) {
    std::uint32_t word = 0;
    if (!KclTryRead32(wordAddr, word)) {
        return false;
    }
    out = static_cast<std::uint16_t>(highHalf ? (word >> 16) : (word & 0xFFFFu));
    return true;
}

// The cached KCollisionV1 header of the course mesh (docs/kcl-runtime.md §2). Internal to the KCL
// reader - everything else in the mod goes through KclRoad.
struct KclMesh {
    std::uint32_t manager = 0;
    std::uint32_t controller = 0;
    std::uint32_t posData = 0, nrmData = 0, prismData = 0, blockData = 0;
    std::uint32_t posCount = 0, nrmCount = 0, prismCount = 0;
    float thickness = 0.0f;
    float areaMinX = 0.0f, areaMinY = 0.0f, areaMinZ = 0.0f;
    std::uint32_t maskX = 0, maskY = 0, maskZ = 0;
    std::uint32_t blockShift = 0, xShift = 0, xyShift = 0;
    bool ready = false;
};

// Section strides, shared by the header reader and the prism decoder (docs/kcl-runtime.md §2).
inline constexpr std::uint32_t kKclVecStride = 12;
inline constexpr std::uint32_t kKclPrismStride = 0x10;

// The course KCL is loaded at the default scale (1.0, the constant at 0x808A6714). The live copy at
// KCLManager+0x04 is per-call scratch and must not be read (docs/kcl-runtime.md §1-2).
inline constexpr float kCourseKclScale = 1.0f;

// A prism's base type is the low five bits of its attribute (docs/kcl-runtime.md §2). Shared so the
// decoder and the classifiers cannot disagree about how wide the field is.
inline constexpr std::uint16_t kKclBaseTypeMask = 0x1F;

const KclMesh& KclMeshData();

// Reads and validates a KCLController header into `out`. Public because an object's collision
// controller holds a KCLController of exactly the same layout (docs/kcl-runtime.md §5), so the
// object overlay builds its meshes with this rather than a second copy of the field offsets.
bool KclReadHeader(std::uint32_t controller, KclMesh& out);

KclSurface KclClassifyBaseType(std::uint8_t baseType);

// False for walls and for the pure triggers and item-only geometry a kart passes through: left in
// the floor set they act as phantom ground (kcl_lib.py:36-40, filtered at kcl_lib.py:133).
bool KclBaseTypeCanBeFloor(std::uint8_t baseType);

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_KCL_MESH_H
