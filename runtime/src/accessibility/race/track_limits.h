#ifndef MKW_ACCESSIBILITY_RACE_TRACK_LIMITS_H
#define MKW_ACCESSIBILITY_RACE_TRACK_LIMITS_H

#include "accessibility/race/edge_map.h"

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
    // Whether the held tone should be sounding, with the surface flag's flap filtered out.
    bool SurfaceSaysOffRoad(bool offRoad, float dtSec);
    // Which ear the cues pan to, held until a new side has lasted longer than a segment jump.
    bool PannedSideIsRight(bool towardsRight, bool haveOffset, float dtSec);

    float mBeepTimer = 0.0f;
    // The magnitude the next beep arms at - the ratchet that turns the beeps into "you are
    // getting closer to the edge" instead of a siren for being outside the CPU's lane.
    float mBeepLevel = 0.0f;
    bool mNearEdge = false;
    bool mHoldingTone = false;
    bool mWasOffRoad = false;
    bool mWasWrongWay = false;
    // How long the surface flag has disagreed with the tone that is currently sounding.
    float mSurfaceHoldSec = 0.0f;
    // The ear the cues are panned to, and how long the measured side has disagreed with it.
    bool mSideRight = false;
    bool mSideKnown = false;
    float mSideHoldSec = 0.0f;
    // Whether the last grade came from the measured road or from the KMP corridor fallback. The
    // two share thresholds but not their rate of change, so the ratchet re-arms when it flips.
    bool mGradeIsReal = false;
    // What ends the road on the side the kart is drifting towards, from the edge cue's last look.
    // Unknown while edge cues are off or the station never probed, which is what makes both the
    // beep variant and the spoken phrase fall back to their plain forms.
    EdgeKind mNearEdgeKind = EdgeKind::Unknown;
};

// Menu preview: plays the held off-road tone as a one-shot, no race state needed.
void PlayEdgeCueDemo();

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_TRACK_LIMITS_H
