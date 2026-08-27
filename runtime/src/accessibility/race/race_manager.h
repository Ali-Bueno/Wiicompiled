#ifndef MKW_ACCESSIBILITY_RACE_RACE_MANAGER_H
#define MKW_ACCESSIBILITY_RACE_RACE_MANAGER_H

namespace a11y::race {

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
