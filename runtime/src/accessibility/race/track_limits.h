#ifndef MKW_ACCESSIBILITY_RACE_TRACK_LIMITS_H
#define MKW_ACCESSIBILITY_RACE_TRACK_LIMITS_H

namespace a11y::race {

struct RaceState;
class CourseMap;
class Handedness;

// Where the edge of the track is, whether the surface has stopped being track, and whether the
// kart is pointing the wrong way. Three separate questions with three separate answers, because
// they do not always agree: a shortcut can be off-road but perfectly legal, and a kart can be
// inside the checkpoint pair while sitting on grass.
//
// One direction language, the player's own: sound sits on the danger side. Approach beeps and
// the off-road held tone both lean towards the side being left, in the KART's frame - the same
// ear the engine leans to under the steer-away convention - so the two agree even when the kart
// is spun sideways.
class TrackLimits {
public:
    void Reset();
    void Tick(const RaceState& state, const CourseMap& map, const Handedness& handedness,
              int station, float dtSec);

private:
    void UpdateEdge(const RaceState& state, const CourseMap& map, const Handedness& handedness,
                    int station, float dtSec);
    void UpdateSurface(const RaceState& state);
    void UpdateWrongWay(const RaceState& state);

    float mBeepTimer = 0.0f;
    // The magnitude the next beep arms at - the ratchet that turns the beeps into "you are
    // getting closer to the edge" instead of a siren for being outside the CPU's lane.
    float mBeepLevel = 0.0f;
    bool mNearEdge = false;
    bool mHoldingTone = false;
    bool mWasOffRoad = false;
    bool mWasWrongWay = false;
};

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_TRACK_LIMITS_H
