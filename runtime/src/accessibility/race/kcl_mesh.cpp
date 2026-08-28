#include "accessibility/race/kcl_mesh.h"

namespace a11y::race {
namespace {

// docs/kcl-runtime.md §1: KCLManager::CreateInstance (0x807C2824) stores the manager here and
// KCLManager::Load (0x807C28D8) fills the KCLController at its +0x00.
constexpr std::uint32_t kKclManagerPtr = 0x809C3C10;
constexpr std::uint32_t kKclManagerController = 0x00;

// docs/kcl-runtime.md §2: the controller keeps the 60-byte header with its four section offsets
// already turned into absolute guest pointers.
constexpr std::uint32_t kCtrlPosData = 0x00;
constexpr std::uint32_t kCtrlNrmData = 0x04;
constexpr std::uint32_t kCtrlPrismData = 0x08;
constexpr std::uint32_t kCtrlBlockData = 0x0C;
constexpr std::uint32_t kCtrlThickness = 0x10;
constexpr std::uint32_t kCtrlAreaMin = 0x14;
constexpr std::uint32_t kCtrlMaskX = 0x20;
constexpr std::uint32_t kCtrlMaskY = 0x24;
constexpr std::uint32_t kCtrlMaskZ = 0x28;
constexpr std::uint32_t kCtrlBlockWidthShift = 0x2C;
constexpr std::uint32_t kCtrlAreaXBlocksShift = 0x30;
constexpr std::uint32_t kCtrlAreaXYBlocksShift = 0x34;
constexpr std::uint32_t kCtrlPrismCount = 0x6C;

// Masked, not merely assumed: a shift by more than 31 is undefined, and the caller's byte is only
// ever meant to be a five-bit field.
constexpr std::uint32_t KclTypeBit(std::uint8_t type) { return 1u << (type & kKclBaseTypeMask); }

// kcl_lib.py:31-33. Drivable means the kart keeps normal grip or better.
constexpr std::uint32_t kKclDrivableTypes =
    KclTypeBit(0x00) | KclTypeBit(0x01) | KclTypeBit(0x05) | KclTypeBit(0x06) | KclTypeBit(0x07) |
    KclTypeBit(0x08) | KclTypeBit(0x0B) | KclTypeBit(0x13) | KclTypeBit(0x15) | KclTypeBit(0x16) |
    KclTypeBit(0x17) | KclTypeBit(0x1D);
constexpr std::uint32_t kKclOffroadTypes = KclTypeBit(0x02) | KclTypeBit(0x03) | KclTypeBit(0x04);
// kcl_lib.py:37-40: WALLS then NONSOLID. Neither can ever be a floor.
constexpr std::uint32_t kKclNonFloorTypes =
    KclTypeBit(0x0C) | KclTypeBit(0x0D) | KclTypeBit(0x0E) | KclTypeBit(0x0F) | KclTypeBit(0x14) |
    KclTypeBit(0x19) | KclTypeBit(0x1C) | KclTypeBit(0x1E) | KclTypeBit(0x1F) | KclTypeBit(0x09) |
    KclTypeBit(0x11) | KclTypeBit(0x12) | KclTypeBit(0x18) | KclTypeBit(0x1A) | KclTypeBit(0x1B);

// A shift wider than a word, or a zero area mask, would make every coordinate look in-bounds.
constexpr std::uint32_t kKclMaxShift = 32;

KclMesh g_kclMesh;

}  // namespace

bool KclReadHeader(std::uint32_t controller, KclMesh& m) {
    if (!KclTryPointer(controller + kCtrlPosData, m.posData) ||
        !KclTryPointer(controller + kCtrlNrmData, m.nrmData) ||
        !KclTryPointer(controller + kCtrlPrismData, m.prismData) ||
        !KclTryPointer(controller + kCtrlBlockData, m.blockData) ||
        !KclTryFloat(controller + kCtrlThickness, m.thickness) ||
        !KclTryVec3(controller + kCtrlAreaMin, m.areaMinX, m.areaMinY, m.areaMinZ) ||
        !KclTryRead32(controller + kCtrlMaskX, m.maskX) ||
        !KclTryRead32(controller + kCtrlMaskY, m.maskY) ||
        !KclTryRead32(controller + kCtrlMaskZ, m.maskZ) ||
        !KclTryRead32(controller + kCtrlBlockWidthShift, m.blockShift) ||
        !KclTryRead32(controller + kCtrlAreaXBlocksShift, m.xShift) ||
        !KclTryRead32(controller + kCtrlAreaXYBlocksShift, m.xyShift) ||
        !KclTryRead32(controller + kCtrlPrismCount, m.prismCount)) {
        return false;
    }
    // The header carries no vertex or normal counts; they fall out of the section pointers exactly
    // as the offline reader derives them (kcl_lib.py:69-71). Prism index 1 is the first real prism,
    // so the normal section ends one stride past prismData.
    if (m.nrmData <= m.posData || m.prismData + kKclPrismStride <= m.nrmData) {
        return false;
    }
    m.posCount = (m.nrmData - m.posData) / kKclVecStride;
    m.nrmCount = (m.prismData + kKclPrismStride - m.nrmData) / kKclVecStride;
    if (m.posCount == 0 || m.nrmCount == 0 || m.prismCount == 0 || m.maskX == 0 || m.maskY == 0 ||
        m.maskZ == 0 || m.blockShift >= kKclMaxShift || m.xShift >= kKclMaxShift ||
        m.xyShift >= kKclMaxShift || !(m.thickness > 0.0f)) {
        return false;
    }
    // Widened so a garbage count cannot wrap the address before the range check sees it.
    const std::uint64_t prismEnd =
        static_cast<std::uint64_t>(m.prismData) + std::uint64_t{m.prismCount} * kKclPrismStride;
    return prismEnd <= 0xFFFFFFFFull &&
           Memory::Contains(static_cast<std::uint32_t>(prismEnd), kKclPrismStride);
}

const KclMesh& KclMeshData() { return g_kclMesh; }

KclSurface KclClassifyBaseType(std::uint8_t baseType) {
    if ((kKclDrivableTypes & KclTypeBit(baseType)) != 0) {
        return KclSurface::Road;
    }
    return (kKclOffroadTypes & KclTypeBit(baseType)) != 0 ? KclSurface::Offroad : KclSurface::Fall;
}

bool KclBaseTypeCanBeFloor(std::uint8_t baseType) {
    return (kKclNonFloorTypes & KclTypeBit(baseType)) == 0;
}

const char* KclSurfaceName(KclSurface surface) {
    switch (surface) {
        case KclSurface::Road:
            return "road";
        case KclSurface::Offroad:
            return "offroad";
        case KclSurface::Fall:
            return "fall";
        default:
            return "void";
    }
}

bool KclRoad::Capture() {
    std::uint32_t manager = 0;
    std::uint32_t controller = 0;
    if (!KclTryPointer(kKclManagerPtr, manager) ||
        !KclTryPointer(manager + kKclManagerController, controller)) {
        Forget();
        return false;
    }
    // Revalidated rather than cached once: the mesh is recreated per course, and re-reading the two
    // pointers every frame makes the exact load path irrelevant (docs/kcl-runtime.md §3).
    if (g_kclMesh.ready && g_kclMesh.manager == manager && g_kclMesh.controller == controller) {
        return true;
    }
    KclMesh next;
    next.manager = manager;
    next.controller = controller;
    if (!KclReadHeader(controller, next)) {
        Forget();
        return false;
    }
    next.ready = true;
    g_kclMesh = next;
    return true;
}

void KclRoad::Forget() { g_kclMesh = KclMesh(); }

bool KclRoad::Ready() { return g_kclMesh.ready; }

}  // namespace a11y::race
