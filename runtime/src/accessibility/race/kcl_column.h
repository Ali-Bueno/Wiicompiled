#ifndef MKW_ACCESSIBILITY_RACE_KCL_COLUMN_H
#define MKW_ACCESSIBILITY_RACE_KCL_COLUMN_H

#include <cstdint>

#include "accessibility/race/kcl_mesh.h"
#include "accessibility/race/kcl_road.h"

namespace a11y::race {

// Walking a vertical column of one KCL mesh and keeping the surface that matters.
//
// Split out of the course probe because the object meshes (§5) need exactly the same walk over a
// different mesh: the caller supplies the mesh, so nothing here knows which one it is.

// Which surface of a column to keep: the one whose height is nearest `y`, within `limit` of it -
// kcl_lib.py's probe_near. That is what follows a banked or sloped surface sideways instead of
// hopping onto another deck, and it is the only rule the edge sweep needs.
struct KclPicker {
    float y = 0.0f;
    float limit = 0.0f;
    KclFloor kept;
    bool haveKept = false;
    float bestDelta = 0.0f;
};

void KclOffer(KclPicker& picker, float surfaceY, std::uint8_t baseType);

// Steps down the column of `mesh` at (x, z), offering every surface it crosses to the picker.
// Coordinates are the mesh's OWN space - world for the course, octree space for an object.
void KclWalkColumn(const KclMesh& mesh, float x, float z, float startY, float stopY,
                   KclPicker& picker);

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_KCL_COLUMN_H
