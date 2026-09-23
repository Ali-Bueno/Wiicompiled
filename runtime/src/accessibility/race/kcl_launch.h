#ifndef MKW_ACCESSIBILITY_RACE_KCL_LAUNCH_H
#define MKW_ACCESSIBILITY_RACE_KCL_LAUNCH_H

#include <cstdint>
#include <vector>

namespace a11y::race {

struct KclMesh;
struct KclFace;

// One floor triangle the kart can leave the ground from and trick off (KclIsLaunchSurface), with
// its three corners in world space. Enumerated from the mesh itself rather than probed under the
// line, so a ramp beside the line or shorter than the station spacing is still found.
struct KclLaunchFace {
    float x[3] = {}, y[3] = {}, z[3] = {};
    std::uint8_t baseType = 0;
};

// Whether prism `index` of `mesh` is a launch surface, from its attribute word alone - one read,
// so a whole course can be screened before any prism is decoded.
bool KclPrismIsLaunch(const KclMesh& mesh, std::uint32_t index);

// The three corners of a decoded prism in the mesh's own space, heights from its plane.
KclLaunchFace KclFaceCorners(const KclFace& face);

// Screens up to `budget` prisms of the course mesh from `begin` (1-based) and appends the launch
// faces. Returns the next index to scan; past the prism count when the course is done.
std::uint32_t KclScanCourseLaunch(std::uint32_t begin, std::uint32_t budget,
                                  std::vector<KclLaunchFace>& out);

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_KCL_LAUNCH_H
