#include "accessibility/race/kcl_objects.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "accessibility/race/kcl_column.h"
#include "accessibility/race/kcl_mesh.h"
#include "accessibility/race/kcl_prism.h"

namespace a11y::race {
namespace {

// docs/kcl-runtime.md §5, all VERIFIED there: the singleton, its master list, and the per-object
// collision controller.
constexpr std::uint32_t kObjectsKclMgrPtr = 0x809C4310;  // ObjectsKCLMgr, CreateInstance 0x8081B428
constexpr std::uint32_t kMgrCount = 0x04;                // u16
constexpr std::uint32_t kMgrList = 0x08;                 // Object**
constexpr std::uint32_t kObjectController = 0xAC;        // ObjectExternKCL::LoadCollision 0x8081AA58
constexpr std::uint32_t kCtrlKcl = 0x00;                 // KCLController*, layout of §2
constexpr std::uint32_t kCtrlLocalToWorld = 0x04;        // Mtx34, row-major
constexpr std::uint32_t kCtrlWorldToLocal = 0x34;        // Mtx34, row-major
constexpr std::uint32_t kCtrlScale = 0x64;               // f32, uniform

// The master list holds at most this many entries (§5). A count past it means the read is garbage,
// not that the course has more objects.
constexpr std::uint32_t kMaxObjects = 100;

constexpr std::uint32_t kPointerStrideBytes = 4;
constexpr std::uint32_t kMtx34Rows = 3;
constexpr std::uint32_t kMtx34Cols = 4;
constexpr std::uint32_t kFloatBytes = 4;

struct KclMtx34 {
    float m[3][4] = {};
};

// A cached object. The list is cached and revalidated; the transform is re-read once per Refresh,
// which is once per frame - a moving object's matrix is only valid for the frame it was read in
// (§5), and no probe within that frame can see a different one.
struct KclObjectEntry {
    std::uint32_t object = 0;
    std::uint32_t controller = 0;
    KclMesh mesh;
    KclMtx34 worldToLocal, localToWorld;
    float scale = 0.0f;
    bool probeable = false;
    // The mesh's own octree area, transformed to world once per snapshot. Rejecting against it
    // costs four compares and saves the twelve reads and nine multiplies a transform would need -
    // the difference between a few thousand guarded reads a tick and well over a million.
    float minX = 0.0f, minY = 0.0f, minZ = 0.0f;
    float maxX = 0.0f, maxY = 0.0f, maxZ = 0.0f;
};

std::vector<KclObjectEntry> g_objects;
std::uint32_t g_objectsManager = 0;
std::uint32_t g_objectsCount = 0;
// Split by cause: a null pointer is an object still loading, a bad header is an entry that is not
// an ExternKCL. One comes back, the other never will.
int g_objectsSkippedPointer = 0;
int g_objectsSkippedHeader = 0;

bool KclReadMtx34(std::uint32_t addr, KclMtx34& out) {
    for (std::uint32_t r = 0; r < kMtx34Rows; ++r) {
        for (std::uint32_t c = 0; c < kMtx34Cols; ++c) {
            if (!KclTryFloat(addr + (r * kMtx34Cols + c) * kFloatBytes, out.m[r][c])) {
                return false;
            }
        }
    }
    return true;
}

void KclApplyMtx34(const KclMtx34& t, float x, float y, float z, float& ox, float& oy, float& oz) {
    ox = t.m[0][0] * x + t.m[0][1] * y + t.m[0][2] * z + t.m[0][3];
    oy = t.m[1][0] * x + t.m[1][1] * y + t.m[1][2] * z + t.m[1][3];
    oz = t.m[2][0] * x + t.m[2][1] * y + t.m[2][2] * z + t.m[2][3];
}

// Whether a world-vertical column is still vertical inside this object. The world up vector maps to
// the matrix's middle column, so an upright object leaves the horizontal parts of it near zero.
// Tilt further than that and a vertical walk reads the object's floors the way it reads a wall,
// which is exactly what kKclMinFloorNormalY already draws the line at - so the same tolerance, and
// the object is skipped rather than answered wrongly.
bool KclUpright(const KclMtx34& worldToLocal) {
    return std::fabs(worldToLocal.m[0][1]) <= kKclMinFloorNormalY &&
           std::fabs(worldToLocal.m[2][1]) <= kKclMinFloorNormalY &&
           std::fabs(worldToLocal.m[1][1]) > kKclMinFloorNormalY;
}

bool KclReadObjectList(std::uint32_t manager, std::uint32_t count,
                       std::vector<KclObjectEntry>& out) {
    std::uint32_t list = 0;
    if (!KclTryPointer(manager + kMgrList, list)) {
        return false;
    }
    g_objectsSkippedPointer = 0;
    g_objectsSkippedHeader = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t object = 0, controller = 0, kcl = 0;
        if (!KclTryPointer(list + i * kPointerStrideBytes, object) ||
            !KclTryPointer(object + kObjectController, controller) ||
            !KclTryPointer(controller + kCtrlKcl, kcl)) {
            ++g_objectsSkippedPointer;  // still loading: the retry below will pick it up
            continue;
        }
        KclObjectEntry entry;
        entry.object = object;
        entry.controller = controller;
        if (!KclReadHeader(kcl, entry.mesh)) {
            ++g_objectsSkippedHeader;  // an entry that is not an ExternKCL is a hypothesis, so it
            continue;                  // is structurally validated and dropped, not trusted (§5)
        }
        entry.mesh.controller = kcl;
        entry.mesh.ready = true;
        out.push_back(entry);
    }
    return true;
}

// The object's own octree area, carried into world space, so a probe can be rejected without
// transforming it. The area bounds come from the mesh header (§2, verified) rather than the
// controller's model-space AABB at +0x74/+0x80, which the notes still mark as a hypothesis - and
// they bound the prisms just as tightly, being exactly the region the octree indexes.
void KclWorldBounds(KclObjectEntry& entry) {
    const KclMesh& m = entry.mesh;
    const float spanX = static_cast<float>(~m.maskX);
    const float spanY = static_cast<float>(~m.maskY);
    const float spanZ = static_cast<float>(~m.maskZ);
    bool first = true;
    for (int corner = 0; corner < 8; ++corner) {
        const float mx = (m.areaMinX + ((corner & 1) ? spanX : 0.0f)) * entry.scale;
        const float my = (m.areaMinY + ((corner & 2) ? spanY : 0.0f)) * entry.scale;
        const float mz = (m.areaMinZ + ((corner & 4) ? spanZ : 0.0f)) * entry.scale;
        float wx = 0.0f, wy = 0.0f, wz = 0.0f;
        KclApplyMtx34(entry.localToWorld, mx, my, mz, wx, wy, wz);
        if (first) {
            entry.minX = entry.maxX = wx;
            entry.minY = entry.maxY = wy;
            entry.minZ = entry.maxZ = wz;
            first = false;
            continue;
        }
        entry.minX = std::min(entry.minX, wx);
        entry.maxX = std::max(entry.maxX, wx);
        entry.minY = std::min(entry.minY, wy);
        entry.maxY = std::max(entry.maxY, wy);
        entry.minZ = std::min(entry.minZ, wz);
        entry.maxZ = std::max(entry.maxZ, wz);
    }
}

// Re-reads every live transform, once. Anything that does not read, or that is tilted so far a
// vertical column cannot probe it, is marked unprobeable for this frame rather than dropped: the
// list itself stays valid and the object can come back next frame.
void KclSnapshotTransforms() {
    for (KclObjectEntry& entry : g_objects) {
        entry.probeable = false;
        if (!KclReadMtx34(entry.controller + kCtrlWorldToLocal, entry.worldToLocal) ||
            !KclReadMtx34(entry.controller + kCtrlLocalToWorld, entry.localToWorld) ||
            !KclTryFloat(entry.controller + kCtrlScale, entry.scale) || !(entry.scale > 0.0f) ||
            !KclUpright(entry.worldToLocal)) {
            continue;
        }
        KclWorldBounds(entry);
        entry.probeable = true;
    }
}

// True while the cached list still describes what is in memory.
bool KclObjectListCurrent(std::uint32_t manager, std::uint32_t count) {
    if (manager != g_objectsManager || count != g_objectsCount) {
        return false;
    }
    // An entry skipped last time is retried, not written off. The controllers are filled in as the
    // objects load, so a Refresh that lands before the mushroom caps have theirs would otherwise
    // leave the gorge reading as a fall for the whole race. The retry cannot run forever: EdgeMap
    // stops calling Refresh once its build is Done, which is what bounds an entry that never reads.
    if (g_objectsSkippedPointer > 0 || g_objectsSkippedHeader > 0) {
        return false;
    }
    for (const KclObjectEntry& entry : g_objects) {
        std::uint32_t controller = 0;
        if (!KclTryPointer(entry.object + kObjectController, controller) ||
            controller != entry.controller) {
            return false;
        }
    }
    return true;
}

}  // namespace

void KclObjects::Forget() {
    g_objects.clear();
    g_objectsManager = 0;
    g_objectsCount = 0;
    g_objectsSkippedPointer = 0;
    g_objectsSkippedHeader = 0;
}

int KclObjects::Count() { return static_cast<int>(g_objects.size()); }

int KclObjects::SkippedCount() { return g_objectsSkippedPointer + g_objectsSkippedHeader; }

int KclObjects::SkippedPointerCount() { return g_objectsSkippedPointer; }

int KclObjects::SkippedHeaderCount() { return g_objectsSkippedHeader; }

bool KclObjects::Refresh() {
    std::uint32_t manager = 0;
    std::uint16_t count = 0;
    if (!KclTryPointer(kObjectsKclMgrPtr, manager) ||
        !KclTryU16(manager + kMgrCount, /*highHalf=*/true, count) || count == 0 ||
        count > kMaxObjects) {
        Forget();
        return false;
    }
    if (!KclObjectListCurrent(manager, count)) {
        g_objects.clear();
        g_objectsManager = manager;
        g_objectsCount = count;
        if (!KclReadObjectList(manager, count, g_objects)) {
            Forget();
            return false;
        }
    }
    KclSnapshotTransforms();
    return !g_objects.empty();
}

void KclObjects::OfferFloors(float x, float z, float startY, float stopY, float referenceY,
                             KclPicker& picker) {
    for (const KclObjectEntry& entry : g_objects) {
        // Four compares against the object's own world bounds throw out every object the column
        // cannot reach, before anything is transformed.
        if (!entry.probeable || x < entry.minX || x > entry.maxX || z < entry.minZ ||
            z > entry.maxZ || startY < entry.minY || stopY > entry.maxY) {
            continue;
        }
        const KclMtx34& worldToLocal = entry.worldToLocal;
        const KclMtx34& localToWorld = entry.localToWorld;
        const float scale = entry.scale;

        // §5's recipe: the world point into local, then divided by the object's own scale, is the
        // space its octree is indexed in. The column's ends go through the same transform, so an
        // upright object's vertical walk covers exactly the world span the caller asked about.
        const auto toMesh = [&](float wy, float& mx, float& my, float& mz) {
            KclApplyMtx34(worldToLocal, x, wy, z, mx, my, mz);
            mx /= scale;
            my /= scale;
            mz /= scale;
        };
        float mx = 0.0f, mz = 0.0f, meshStart = 0.0f, meshStop = 0.0f, meshRef = 0.0f;
        float spareX = 0.0f, spareZ = 0.0f;
        toMesh(startY, mx, meshStart, mz);
        toMesh(stopY, spareX, meshStop, spareZ);
        toMesh(referenceY, spareX, meshRef, spareZ);
        if (!(meshStart > meshStop)) {
            continue;  // this object's vertical runs the other way; a downward walk covers nothing
        }

        // One surface per object, the one nearest the reference: an overlay is a cap to stand on,
        // not a stack. The span is the whole column, so the limit never excludes anything inside
        // it - which of them survives is then the world picker's decision, not this one's.
        KclPicker local;
        local.y = meshRef;
        local.limit = (meshStart - meshStop);
        KclWalkColumn(entry.mesh, mx, mz, meshStart, meshStop, local);
        if (!local.haveKept) {
            continue;
        }

        // Back to world through the local-to-world matrix, undoing the scale first (§5).
        float wx = 0.0f, wy = 0.0f, wz = 0.0f;
        KclApplyMtx34(localToWorld, mx * scale, local.kept.y * scale, mz * scale, wx, wy, wz);
        KclOffer(picker, wy, local.kept.baseType);
    }
}

}  // namespace a11y::race
