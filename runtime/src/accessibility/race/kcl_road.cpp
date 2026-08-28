#include "accessibility/race/kcl_road.h"

#include "accessibility/race/kcl_column.h"
#include "accessibility/race/kcl_mesh.h"
#include "accessibility/race/kcl_objects.h"

namespace a11y::race {

KclFloor KclRoad::ProbeFloorNear(float x, float z, float referenceY, float window) {
    KclFloor out;
    if (!Ready()) {
        return out;
    }
    const float start = referenceY + window;
    const float stop = referenceY - window;
    KclPicker picker;
    picker.y = referenceY;
    picker.limit = window;
    KclWalkColumn(KclMeshData(), x, z, start, stop, picker);
    KclObjects::OfferFloors(x, z, start, stop, referenceY, picker);
    return picker.haveKept ? picker.kept : out;
}

}  // namespace a11y::race
