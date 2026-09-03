#ifndef MKW_ACCESSIBILITY_RACE_KMP_READER_H
#define MKW_ACCESSIBILITY_RACE_KMP_READER_H

#include <cstdint>
#include <vector>

namespace a11y::race {

// A KMP checkpoint: the track's left and right edge at one station along the lap, in the
// horizontal plane. These are lap-validation volumes and are much wider than the road, so they are
// used for progress and for the left/right convention, never for geometry.
struct Checkpoint {
    float leftX = 0.0f, leftZ = 0.0f;
    float rightX = 0.0f, rightZ = 0.0f;
};

// The most successors a route point can have. A raw ENPH group states up to six next groups, so a
// point ending a group can branch at most that many ways.
inline constexpr int kMaxRouteLinks = 6;

// One point of the AI route. Far denser than the checkpoint list on most courses, which is what
// makes it usable for describing the shape of a corner.
//
// The successor list is the whole reason this is usable at all. ENPT entry order is not a path -
// the points are grouped and the groups branch - which is why walking the list by index measured a
// lap twice as long as the real one. KMP::Holder<ENPT>::InitLinks has already flattened the ENPH
// branch groups into a per-point successor array, so following `next` follows the route the CPUs
// actually drive.
struct RoutePoint {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    // The game's own corridor half-width is 50 times this. Always ENPT's value, even for a point
    // that came from ReadCourseItemRoute - ITPT's own field at the same struct offset means
    // something else entirely (docs/mkwii-track-format.md).
    float range = 0.0f;
    // ENPT setting2, the author's drift instruction to the CPU at this point: 1 end the drift,
    // 2 forbid drifting, 3 drift here whatever the geometry says. Zero on an item-route point,
    // which has no such field.
    std::uint8_t driftSetting = 0;
    std::uint8_t next[kMaxRouteLinks] = {};
    std::uint8_t nextCount = 0;
};

// The value of RoutePoint::driftSetting that orders a drift: AI::ENPTSettingsHolder::MustDrift
// (0x8073EC70) is `setting2 == 3`.
inline constexpr std::uint8_t kRouteForceDrift = 3;

// Non-zero while a course is loaded: the KMP manager pointer, which has exactly two writers in the
// whole game, one setting it and one clearing it on scene exit. Says nothing about *which* course -
// use CourseSignature for that.
std::uint32_t CourseToken();

// Identifies the *parsed course*, not just the manager object. Non-zero once the checkpoints are
// readable, and a different value for a different course.
//
// The manager pointer alone is not enough: it is heap-allocated per course, so two consecutive
// races can land on the same address, and if no frame is presented while it is null the change
// goes unnoticed and the previous course's geometry is used for the whole next race.
std::uint64_t CourseSignature();

// Reads the checkpoint list of the loaded course. False if no course is loaded or the list did not
// read cleanly, in which case `out` is left empty.
//
// Kept in the KMP's own entry order, which matters twice over: it is the order the checkpoint
// groups run in, and it is the order the game's own per-player checkpoint index refers to, so that
// index can be used directly instead of searching for the nearest station. The cost is that a
// course whose groups branch and rejoin may order its side path oddly.
bool ReadCourseCheckpoints(std::vector<Checkpoint>& out);

// Reads the AI route (KMP ENPT) of the loaded course, successor links included.
bool ReadCourseRoute(std::vector<RoutePoint>& out);

// Reads the item route (KMP ITPT) the same way - the line shells and Bullet Bill follow. Offline
// it measured more central and smoother than the CPUs' route on most courses, which is why it is
// the guide's preferred backbone.
//
// Also reads ENPT internally, purely to fill in each point's `range`: ITPT's own field at that
// struct offset is not a corridor width (docs/mkwii-track-format.md), so the width has nowhere
// else to come from. Fails if ENPT does not read either, since then the route would have no width
// at all - the caller already falls back to the CPU/ENPT route in that case.
bool ReadCourseItemRoute(std::vector<RoutePoint>& out);

// The lap length the game itself computed from the checkpoint midpoints, cached on the CKPT
// section at parse time. A sanity check with teeth: the first attempt at a centre line measured a
// lap twice this long, and this one number would have said so on the first frame.
bool ReadCourseLapLength(float& out);

// The route point the grid feeds into, which is where a walk of the route has to start. The game
// caches it on the first start-point holder and seeds its own CPU route controllers from it.
bool ReadRouteStartPoint(std::uint8_t& out);

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_KMP_READER_H
