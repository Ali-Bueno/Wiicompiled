#include "accessibility/race/kcl_column.h"

#include <cmath>

#include "accessibility/race/kcl_prism.h"

namespace a11y::race {
namespace {

// An octree word with the top bit set is a leaf and the rest is the offset of its u16 prism list;
// otherwise the word is the offset of the node's eight children (docs/kcl-runtime.md §2).
constexpr std::uint32_t kOctreeLeafFlag = 0x80000000u;
constexpr std::uint32_t kOctreeOffsetMask = 0x7FFFFFFFu;
constexpr std::uint32_t kOctreeNodeStride = 4;

// Safety bounds only. No MKWii leaf holds this many prisms and no column stacks this many leaves;
// they exist so a corrupt pointer cannot spin the frame tick.
constexpr int kMaxLeafPrisms = 1024;
constexpr int kMaxColumnCells = 64;

struct KclLeaf {
    std::uint32_t list = 0;
    std::int32_t bottomLocalY = 0;
};

bool KclLocalXZ(const KclMesh& m, float x, float z, std::uint32_t& ux, std::uint32_t& uz) {
    if (!std::isfinite(x) || !std::isfinite(z)) {
        return false;
    }
    // Computed in double so the range test is exact for every u32 and the conversion below can
    // never be handed a float outside the integer range. The mask clears every bit inside the area,
    // so its complement is the largest local coordinate the area holds: testing against it is the
    // same rejection as the game's own `local & mask`, without the undefined cast on the way.
    const double localX = static_cast<double>(x) - m.areaMinX;
    const double localZ = static_cast<double>(z) - m.areaMinZ;
    if (!(localX >= 0.0) || !(localZ >= 0.0) || localX > static_cast<double>(~m.maskX) ||
        localZ > static_cast<double>(~m.maskZ)) {
        return false;
    }
    ux = static_cast<std::uint32_t>(localX);
    uz = static_cast<std::uint32_t>(localZ);
    return true;
}

// docs/kcl-runtime.md §2, the body of KCLController::FindTrianglesList (0x807BE030). The shift the
// descent ends on is also the leaf's size, which is what lets a vertical walk step to the next cell
// down instead of guessing a stride.
bool KclFindLeaf(const KclMesh& m, std::uint32_t ux, std::uint32_t uy, std::uint32_t uz,
                 KclLeaf& out) {
    if ((uy & m.maskY) != 0) {
        return false;
    }
    std::uint32_t shift = m.blockShift;
    std::uint32_t node = m.blockData;
    std::uint32_t offset =
        (((uz >> shift) << m.xyShift) | ((uy >> shift) << m.xShift) | (ux >> shift)) *
        kOctreeNodeStride;
    for (std::uint32_t depth = 0; depth <= m.blockShift; ++depth) {
        std::uint32_t value = 0;
        if (!KclTryRead32(node + offset, value)) {
            return false;
        }
        if ((value & kOctreeLeafFlag) != 0) {
            out.list = node + (value & kOctreeOffsetMask);
            out.bottomLocalY = static_cast<std::int32_t>((uy >> shift) << shift);
            return true;
        }
        node += value;
        if (shift == 0) {
            return false;
        }
        --shift;
        offset = ((((uz >> shift) & 1u) << 2) | (((uy >> shift) & 1u) << 1) | ((ux >> shift) & 1u)) *
                 kOctreeNodeStride;
    }
    return false;
}

void KclScanLeaf(const KclMesh& m, const KclLeaf& leaf, float x, float z, KclPicker& picker) {
    // The list is u16 prism indices, 1-based and zero-terminated. Guest halfwords are addressed by
    // their containing word, and the one at the word's own address is the high half.
    //
    // The pointer the descent returns is biased by -2: the game's own consumer advances its cursor
    // BEFORE its first read (shard_7876885ae2c628b5c1e906c3.cpp loc_807C0820 - it loads the cursor
    // from controller+0x68, adds 2, stores it back, and only then reads the halfword). Starting at
    // the raw pointer reads the previous list's terminator and every leaf comes back empty.
    std::uint32_t entry = leaf.list + sizeof(std::uint16_t);
    for (int n = 0; n < kMaxLeafPrisms; ++n) {
        std::uint16_t index = 0;
        if (!KclTryU16(entry & ~3u, (entry & 2u) == 0, index) || index == 0) {
            return;
        }
        entry += sizeof(std::uint16_t);
        KclFace face;
        if (!KclDecodePrism(m, index, face) || !KclCoversXZ(face, x, z)) {
            continue;
        }
        KclOffer(picker, KclHeightAt(face, x, z), face.baseType);
    }
}

}  // namespace

void KclOffer(KclPicker& p, float surfaceY, std::uint8_t baseType) {
    if (!std::isfinite(surfaceY)) {
        return;
    }
    const float delta = std::fabs(surfaceY - p.y);
    if (delta > p.limit || (p.haveKept && delta >= p.bestDelta)) {
        return;
    }
    p.kept.hit = true;
    p.kept.y = surfaceY;
    p.kept.baseType = baseType;
    p.kept.category = KclClassifyBaseType(baseType);
    p.bestDelta = delta;
    p.haveKept = true;
}

// A prism is listed in every leaf its box overlaps, so the leaf holding a surface point also holds
// that prism: walking the column downwards and stopping at the first cell that yields a surface
// finds the highest one below the start.
void KclWalkColumn(const KclMesh& m, float x, float z, float startY, float stopY,
                   KclPicker& picker) {
    std::uint32_t ux = 0, uz = 0;
    if (!m.ready || !std::isfinite(startY) || !KclLocalXZ(m, x, z, ux, uz)) {
        return;
    }
    // The mask clears every bit inside the area, so its complement is the highest local Y in it.
    const double topLocal = static_cast<double>(~m.maskY);
    const double startLocal = static_cast<double>(startY) - m.areaMinY;
    // Rejects NaN and anything below the area, and keeps the conversion in range.
    if (!(startLocal >= 0.0)) {
        return;
    }
    std::int64_t localY = static_cast<std::int64_t>(startLocal > topLocal ? topLocal : startLocal);
    const double stopLocal = static_cast<double>(stopY) - m.areaMinY;
    for (int step = 0; step < kMaxColumnCells && localY >= 0; ++step) {
        KclLeaf leaf;
        if (!KclFindLeaf(m, ux, static_cast<std::uint32_t>(localY), uz, leaf)) {
            return;
        }
        KclScanLeaf(m, leaf, x, z, picker);
        // The whole span is walked: a nearer surface can sit in any cell of it, and a prism listed
        // in one leaf can have its surface in another.
        if (static_cast<double>(leaf.bottomLocalY) <= stopLocal) {
            return;
        }
        localY = static_cast<std::int64_t>(leaf.bottomLocalY) - 1;
    }
}

}  // namespace a11y::race
