#ifndef MKW_ACCESSIBILITY_RACE_RAMP_MAP_H
#define MKW_ACCESSIBILITY_RACE_RAMP_MAP_H

#include <vector>

namespace a11y::race {

class CourseMap;

// One surface along the line the kart can launch and trick from: a boost ramp, a jump pad (the
// bouncy mushrooms) or a trickable slope, as the KCL marks it (KclIsLaunchSurface).
struct Ramp {
    float arcStart = 0.0f;  // where it begins along the line
    float length = 0.0f;    // how far along the line it runs
    // Its lateral centre where it begins, in world space, so it stays put if the line moves.
    float entryX = 0.0f, entryZ = 0.0f;
    // Half its extent across the road: how far off its centre the kart's centre can be and still
    // be on it.
    float halfWidth = 0.0f;
};

// Every launch surface of the course, found in the collision mesh itself - the course KCL and the
// object KCLs - and placed along the line. Sampling under the line missed every ramp beside it and
// any ramp shorter than the station spacing (docs/trick-ramp-cue.md), so nothing here is sampled.
//
// Built across a few ticks once the edge sweep is done, cached for the course, rebound when the
// line's road shift is applied, dropped with the course map.
struct RampMap {
    static void Tick(const CourseMap& map);
    static void Reset();
    static bool Ready();
    // In order of arcStart.
    static const std::vector<Ramp>& Ramps();
};

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_RAMP_MAP_H
