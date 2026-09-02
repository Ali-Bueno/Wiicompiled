#ifndef MKW_ACCESSIBILITY_RACE_TRACK_LIMITS_H
#define MKW_ACCESSIBILITY_RACE_TRACK_LIMITS_H

#include "accessibility/race/edge_map.h"

namespace a11y::race {

struct RaceState;
class CourseMap;
class Handedness;

// The beeps close up as the edge nears, which is the urgency signal. Here rather than in the .cpp
// because every flap guard in this module is derived from the near interval.
inline constexpr float kIntervalFarSec = 0.45f;
inline constexpr float kIntervalNearSec = 0.09f;

// One beep interval at full urgency is the shortest gap this cue family already treats as two
// separate events, so a flag that reads the other way for less than that is flap, not a change.
inline constexpr float kLimitDebounceSec = kIntervalNearSec;

// Where the edge of the track is, whether the surface has stopped being track, and whether the
// kart is pointing the wrong way. Three separate questions with three separate answers, because
// they do not always agree: a shortcut can be off-road but perfectly legal, and a kart can be
// inside the checkpoint pair while sitting on grass.
//
// One direction language, the player's own: sound sits on the danger side. Approach beeps and
// the off-road held tone both lean towards the side being left, in the KART's frame - the same
// ear the engine leans to under the steer-away convention - so the two agree even when the kart
// is spun sideways.
//
// Every lead in here is a TIME, never a distance: the edge grade rides the margin the kart will
// have one anticipation horizon from now, so the warning is the same at 50cc and at 500cc.
class TrackLimits {
public:
    void Reset();
    void Tick(const RaceState& state, const CourseMap& map, const Handedness& handedness,
              int station, float dtSec);

private:
    void UpdateEdge(const RaceState& state, const CourseMap& map, const Handedness& handedness,
                    int station, float dtSec, bool offRoad);
    void UpdateSurface(bool offRoad);
    void UpdateWrongWay(bool wrongWay, float dtSec);
    // Whether the surface has really changed, with the game's frame-to-frame flap filtered out.
    bool SurfaceSaysOffRoad(bool offRoad, float dtSec);
    // Which ear the cues pan to, held until a new side has lasted longer than a segment jump.
    bool PannedSideIsRight(bool towardsRight, bool haveOffset, float dtSec);

    float mBeepTimer = 0.0f;
    // The grade at the last beep. A rate limit, not a ladder: while the grade keeps rising the
    // beeps keep coming at whatever interval the current nearness asks for.
    float mBeepLevel = 0.0f;
    // The grade last frame, so a whole perceptual step crossed inside one frame forces a beep.
    float mLastMagnitude = 0.0f;
    bool mNearEdge = false;
    bool mHoldingTone = false;
    // The debounced surface. Both the held tone and the spoken change are driven off this one
    // value, so a kerb bounce cannot warble the tone or queue a backlog of "off road / on road".
    bool mSurfaceOffRoad = false;
    // How long the surface flag has disagreed with the debounced value above.
    float mSurfaceHoldSec = 0.0f;
    // What the narration last said, which is a different question from what the surface reads.
    bool mWasOffRoad = false;
    // Wrong way, debounced the same way, plus the countdown to the next re-announcement.
    bool mWasWrongWay = false;
    float mWrongWayHoldSec = 0.0f;
    float mWrongWaySaySec = 0.0f;
    // The ear the cues are panned to, and how long the measured side has disagreed with it.
    bool mSideRight = false;
    bool mSideKnown = false;
    float mSideHoldSec = 0.0f;
    // Whether the last grade came from the measured road or from the KMP corridor fallback. The
    // two share thresholds but not their rate of change, so the limiter re-arms when it flips.
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
