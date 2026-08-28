#ifndef MKW_ACCESSIBILITY_RACE_RACE_MANAGER_H
#define MKW_ACCESSIBILITY_RACE_RACE_MANAGER_H

namespace a11y::race {

// How long a course-dependent read may keep retrying before it accepts failure and says so - about
// ten seconds at sixty frames. Shared so the route retry and the collision-mesh survey give up on
// the same schedule instead of each carrying its own copy of the number.
inline constexpr int kCourseSettleFrames = 600;

// Once per presented frame. Reads the race state once, resolves where on the course the player is,
// and hands both to each race service - so no two services can disagree about where the kart is,
// and the guest memory walk happens once rather than per feature.
void Tick();

void Reset();

// Drops the built course map so the next Tick rebuilds it from the current config - how a
// line_source change in a hot-reloaded Config.toml takes effect mid-race.
void InvalidateCourseMap();

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_RACE_MANAGER_H
