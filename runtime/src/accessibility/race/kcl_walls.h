#ifndef MKW_ACCESSIBILITY_RACE_KCL_WALLS_H
#define MKW_ACCESSIBILITY_RACE_KCL_WALLS_H

namespace a11y::race {

// Whether a wall of the course mesh that stops a kart crosses the horizontal step from (x0, z0)
// to (x1, z1) at the height of a kart body standing on `floorY`, and how far along the step it
// does (0..1). The floor sweep alone cannot see a wall: the floor runs on under a rail or a
// retaining wall, and a line measured that way was placed 40 units from one (N64 Luigi Raceway,
// road corpus 2026-09-23).
bool KclWallAcross(float x0, float z0, float x1, float z1, float floorY, float& t);

// Whether a kart driving straight from (x0, z0) to (x1, z1) on a floor at `floorY` would meet a
// wall on the way: KclWallAcross over the path in lateral-march steps, so a long path cannot skip
// the leaves between its ends.
bool KclWallBetween(float x0, float z0, float x1, float z1, float floorY);

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_KCL_WALLS_H
