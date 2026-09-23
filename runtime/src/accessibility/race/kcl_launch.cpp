#include "accessibility/race/kcl_launch.h"

#include <algorithm>

#include "accessibility/race/kcl_mesh.h"
#include "accessibility/race/kcl_prism.h"

namespace a11y::race {

bool KclPrismIsLaunch(const KclMesh& mesh, std::uint32_t index) {
    std::uint16_t attribute = 0;
    if (!KclTryPrismAttribute(mesh, index, attribute)) {
        return false;
    }
    return KclIsLaunchSurface(static_cast<std::uint8_t>(attribute & kKclBaseTypeMask),
                              (attribute & kKclTrickableBit) != 0);
}

KclLaunchFace KclFaceCorners(const KclFace& face) {
    KclLaunchFace out;
    const float xs[3] = {face.ax, face.bx, face.cx};
    const float zs[3] = {face.az, face.bz, face.cz};
    for (int k = 0; k < 3; ++k) {
        out.x[k] = xs[k];
        out.z[k] = zs[k];
        out.y[k] = KclHeightAt(face, xs[k], zs[k]);
    }
    out.baseType = face.baseType;
    return out;
}

std::uint32_t KclScanCourseLaunch(std::uint32_t begin, std::uint32_t budget,
                                  std::vector<KclLaunchFace>& out) {
    const KclMesh& mesh = KclMeshData();
    if (!mesh.ready) {
        return begin;
    }
    const std::uint32_t end = std::min(mesh.prismCount + 1, begin + budget);
    for (std::uint32_t i = std::max(begin, 1u); i < end; ++i) {
        KclFace face;
        if (KclPrismIsLaunch(mesh, i) && KclDecodePrism(mesh, i, face)) {
            out.push_back(KclFaceCorners(face));
        }
    }
    return end;
}

}  // namespace a11y::race
