#ifndef MKW_ACCESSIBILITY_RACE_KCL_ROAD_H
#define MKW_ACCESSIBILITY_RACE_KCL_ROAD_H

#include <cstdint>

namespace a11y::race {

// The step every lateral march takes, and the resolution every edge distance is measured to. Well
// under the narrowest MKWii shoulder, so no edge is stepped over. Shared, because a station whose
// road is thinner than one step is a failed probe rather than a road, and that test must use the
// same number the march does.
inline constexpr float kKclLateralStepUnits = 50.0f;

// What a KCL surface means to a kart. The base-type sets behind this are the ones the offline
// prototype was validated with (scratchpad kcl_lib.py:31-50, from the KCL_flag wiki page).
enum class KclSurface : std::uint8_t {
    None,     // no floor under the sample: a hole, or outside the collision area
    Road,     // drivable - normal grip or better
    Offroad,  // solid but penalised
    Fall,     // solid fall or out-of-bounds trigger
};

const char* KclSurfaceName(KclSurface surface);

struct KclFloor {
    bool hit = false;
    float y = 0.0f;
    std::uint8_t baseType = 0;
    KclSurface category = KclSurface::None;
};

// One side of the road, measured outwards from a point on the line.
//
// `beyond` is the finding that matters for the cue and it is deliberately not `cause`: on Mushroom
// Gorge's caps the prism that ends the road is a 0x1E SpecialWall - the cap's rim - but what lies
// past it is void, and the player goes OVER it rather than into it. Classifying by the edge prism
// alone called 11 of kinoko's 13 fall-bounded stations a wall, so what is measured is what is out
// there, not what the boundary is made of.
struct KclEdge {
    float distance = 0.0f;                // furthest sample that was still Road
    KclSurface cause = KclSurface::None;  // what ended the road; None means the floor ran out
    bool openEnded = false;               // still Road at the sampling limit
    KclSurface beyond = KclSurface::None;  // the surface past the edge
    bool beyondHit = false;                // false means nothing out there at all: a drop
};

struct KclEdges {
    bool valid = false;
    KclFloor centre;
    KclEdge left;
    KclEdge right;
};

// Read-only view of the collision mesh the game has already loaded and parsed.
//
// Everything here parses guest memory itself. The game's own queries mutate the controller's cached
// query state, so calling them from the frame tick would poison the collision cache and change
// gameplay (docs/kcl-runtime.md §4).
struct KclRoad {
    // Resolves the controller and caches its header. Cheap enough to call every frame: it re-reads
    // the two pointers and only re-caches when they move.
    static bool Capture();
    static void Forget();
    static bool Ready();

    // The floor whose height is closest to referenceY within +/-window - what follows a banked or
    // sloped surface sideways instead of hopping onto another deck (kcl_lib.py:207-215).
    static KclFloor ProbeFloorNear(float x, float z, float referenceY, float window);

    // Walks outwards along +/-(rightX, rightZ) until the floor stops being Road, then looks past
    // the edge to say what is actually there. Each side also stops at its own reach, because a
    // surface test alone cannot tell this stretch of road from a paved area joined to it - the
    // caller supplies where its own stretch ends (CourseMap::LateralReach).
    static KclEdges ProbeEdges(float x, float y, float z, float rightX, float rightZ,
                               float leftReach, float rightReach);

    // The signed lateral distance from this point to the nearest Road, searching both ways out to
    // `limit`. Zero when the point already stands on road. False when no road is within the limit,
    // which is the caller's cue to leave the point exactly where the course authored it.
    static bool FindRoad(float x, float y, float z, float rightX, float rightZ, float limit,
                         float& shiftOut);

    // The vertical reach of an unanchored probe, which is also the drop that counts as a fall.
    static float ProbeReach();
};

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_KCL_ROAD_H
