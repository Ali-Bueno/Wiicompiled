#ifndef MKW_ACCESSIBILITY_RACE_ITEM_BEACON_H
#define MKW_ACCESSIBILITY_RACE_ITEM_BEACON_H

namespace a11y::race {

struct RaceState;
class CourseMap;
class Handedness;

// A repeating blip on the nearest item box the player could still collect.
//
// It goes silent the moment the player is holding something, because then the box is no longer
// worth steering for and the cue would only compete with the driving assists. It is a short cue, so
// it may pan on its own without becoming a second sustained direction language.
//
// It describes; it does not steer. The box has to be driven into exactly as a sighted player would.
class ItemBeacon {
public:
    void Reset();
    void Tick(const RaceState& state, const CourseMap& map, const Handedness& handedness,
              int station, float dtSec);

private:
    float mBlipTimer = 0.0f;
};

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_ITEM_BEACON_H
