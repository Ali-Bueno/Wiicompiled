#ifndef MKW_ACCESSIBILITY_RACE_EDGE_MAP_H
#define MKW_ACCESSIBILITY_RACE_EDGE_MAP_H

#include <cstdint>
#include <vector>

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

// The warning distance, as a share of the course's real half-width: the track-limit cue starts
// this far from an edge, and the line is placed no closer to a wall or the grass than this, so
// "on the line" is always just outside the warning and any drift towards the edge is heard at
// once. One number for both, because the first racing line placed with only the kart's own
// half-width as clearance put "on the line" and "on the grass" 103 units apart (2026-09-02). A
// DROP keeps this share of its own local half-width clear as well, whichever is larger.
inline constexpr float kEdgeOnsetRealFraction = 0.5f;

// The real road at one station, in world units either side of the line. The two sides are valid
// independently: a route point that sits within one march step of the left boundary still has a
// perfectly good right-hand measurement, and discarding it would silence the cue exactly on the
// narrow ledges where it matters most.
//
// The distances are always measured from the line the COURSE MAP has, never from wherever the
// sweep happened to stand: from the authored station while the repair is only a proposal, and from
// the moved station once EdgeMap::ConfirmShift says the map kept it. A side the authored point
// already stands outside reads negative, which is the truth about that point.
struct StationEdges {
    bool leftValid = false;
    bool rightValid = false;
    float leftDistance = 0.0f;
    float rightDistance = 0.0f;
    EdgeKind leftKind = EdgeKind::Unknown;
    EdgeKind rightKind = EdgeKind::Unknown;
    // How far this station should move towards the track's right to sit on asphalt and inside its
    // own safe band - the TOTAL correction from where the course authored it. Zero on a station the
    // route already placed well, which is most of them.
    float shift = 0.0f;

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
    // Call once per frame with the course map, the player kart's body half-width (0 while no kart
    // reads: the line waits for it) and the guest frame's duration, which bounds how long the line
    // solve may run. Does nothing once the course is measured and the line placed.
    static void Tick(const CourseMap& map, float kartHalfWidth, float frameSec);
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

    // The per-station shift towards the track's right that places the racing line inside the real
    // road, in world units and in station order. Empty until the line is placed. The course map applies it
    // to its own geometry AND to the route it measures the lateral offset against, because those
    // two must stay one line: aiming at one while grading position against the other is the bug
    // that made a player following the guide read as drifting inside every corner.
    static const std::vector<float>& Shifts();

    // Told by CourseMap::ApplyRoadShift whether it kept the shift above. On `true` the measured
    // distances are rebased onto the moved stations; on `false` they stay on the authored ones.
    // Either way SideAtArc and the course map then describe the same line, which they did not when
    // the repair was reverted and the distances still measured from where it would have gone.
    // Idempotent: only the first call after a build decides.
    static void ConfirmShift(bool applied);

    // True when at least one station had to be moved - the course authored part of its line off
    // the road, which is worth saying once rather than silently correcting.
    static bool AnyShift();

    // The course's real road half-width in world units, or 0 before the map completes. This is the
    // length scale anything measured in "track widths" must use: the KMP corridor is the CPU
    // drivers' lane, and on a custom course it can be a small fraction of the asphalt.
    static float MedianHalfWidth();

    // kEdgeOnsetRealFraction of that: the distance from an edge at which the cue starts and inside
    // which the line is never placed. Zero before the map completes.
    static float WarningDistance();
};

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_EDGE_MAP_H
