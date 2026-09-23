#ifndef MKW_ACCESSIBILITY_RACE_TRICK_BEACON_H
#define MKW_ACCESSIBILITY_RACE_TRICK_BEACON_H

namespace a11y::race {

struct RaceState;
class CourseMap;
class Handedness;

// A repeating rising blip on the next surface along the safe line the kart can trick off: a boost
// ramp, a jump pad or a trickable slope - Mushroom Gorge's caps included, since they are jump pads.
//
// The surfaces come from the once-per-race edge sweep (StationEdges::launch), so a race costs no
// guest reads here. A run of consecutive launch stations is one target, its first station, and the
// run the kart is already on is never its own target.
//
// It describes; it does not steer. The ramp has to be driven onto and the trick timed by the player.
class TrickBeacon {
public:
    void Reset();
    void Tick(const RaceState& state, const CourseMap& map, const Handedness& handedness,
              int station, float dtSec);

private:
    float mBlipTimer = 0.0f;
};

// Menu preview: plays the near, centred trick-ramp blip as a one-shot.
void PlayTrickRampCueDemo();

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_TRICK_BEACON_H
