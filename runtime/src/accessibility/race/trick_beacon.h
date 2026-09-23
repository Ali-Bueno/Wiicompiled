#ifndef MKW_ACCESSIBILITY_RACE_TRICK_BEACON_H
#define MKW_ACCESSIBILITY_RACE_TRICK_BEACON_H

namespace a11y::race {

struct RaceState;
class CourseMap;
class Handedness;

// A repeating rising blip on the next surface along the line the kart can trick off: a boost ramp,
// a jump pad or a trickable slope - Mushroom Gorge's caps included, since they are jump pads.
//
// The ramps come from RampMap, which reads them out of the collision mesh once per course, so a
// ramp beside the line counts as much as one on it. The pan says where the ramp's centre is across
// the road relative to the kart: centred means lined up with it (player, 2026-09-23). When two
// ramps start abreast, the one nearer the kart across the road is the target, held until passed.
//
// It describes; it does not steer. The ramp has to be driven onto and the trick timed by the player.
class TrickBeacon {
public:
    void Reset();
    void Tick(const RaceState& state, const CourseMap& map, const Handedness& handedness,
              int station, float dtSec);

private:
    float mBlipTimer = 0.0f;
    int mTarget = -1;  // index into RampMap::Ramps()
};

// Menu preview: plays the near, centred trick-ramp blip as a one-shot.
void PlayTrickRampCueDemo();

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_TRICK_BEACON_H
