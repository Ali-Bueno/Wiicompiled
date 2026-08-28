#ifndef MKW_ACCESSIBILITY_RACE_EDGE_MAP_H
#define MKW_ACCESSIBILITY_RACE_EDGE_MAP_H

#include <cstdint>

namespace a11y::race {

class CourseMap;

// What ends the road on one side of a station.
enum class EdgeKind : std::uint8_t {
    Unknown,  // the probe did not read: the caller falls back to the KMP corridor
    Open,     // still road at the march cap - no edge worth warning about on this side
    Offroad,  // grass, dirt, sand: slow, recoverable
    Wall,     // solid, with road still behind it: a scrape
    Fall,     // void or a drop past recovery - the one the player must hear differently
};

const char* EdgeKindName(EdgeKind kind);

// The real road at one station, in world units either side of the line. The two sides are valid
// independently: a route point that sits within one march step of the left boundary still has a
// perfectly good right-hand measurement, and discarding it would silence the cue exactly on the
// narrow ledges where it matters most.
struct StationEdges {
    bool leftValid = false;
    bool rightValid = false;
    float leftDistance = 0.0f;
    float rightDistance = 0.0f;
    EdgeKind leftKind = EdgeKind::Unknown;
    EdgeKind rightKind = EdgeKind::Unknown;

    bool Valid(bool right) const { return right ? rightValid : leftValid; }
    float Distance(bool right) const { return right ? rightDistance : leftDistance; }
    EdgeKind Kind(bool right) const { return right ? rightKind : leftKind; }
};

// The real road edges of the loaded course, measured once from the collision mesh.
//
// The KMP corridor the course map is built on is the CPU drivers' lane, not the road: offline it
// measured 2-4x narrower than the asphalt, so a cue anchored to it fires while the player is still
// on track. This measures the road itself - course.kcl merged with the object KCLs, since on
// Mushroom Gorge the only thing to stand on across the gorge is an object.
//
// Built a couple of stations per tick so it never costs a frame spike, cached for the course, and
// dropped whenever the course map is (which is also what a line_source change does).
struct EdgeMap {
    // Call once per frame with the course map. Does nothing once the course is measured.
    static void Tick(const CourseMap& map);
    static void Reset();

    // The measured edges at a station, or an invalid entry when that station never read - the
    // caller must then fall back to the corridor rather than go silent.
    static StationEdges At(int station);

    // The real road on one side at a continuous arc position, blended between the two stations
    // that bracket it so everything built on it ramps instead of stepping at station boundaries.
    // False when either bracketing station never read that side, which the caller must treat as
    // "no scale here" rather than substituting one - the KMP corridor is 2-4x too narrow to stand
    // in for this. The kind is categorical, so it takes the nearer station's answer, except that a
    // drop on either side of the kart wins: the harsher warning is the safe way to be wrong.
    static bool SideAtArc(const CourseMap& map, float arc, bool right, float& distance,
                          EdgeKind& kind);

    static bool Ready();
};

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_EDGE_MAP_H
