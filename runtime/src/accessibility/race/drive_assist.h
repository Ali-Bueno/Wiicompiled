#ifndef MKW_ACCESSIBILITY_RACE_DRIVE_ASSIST_H
#define MKW_ACCESSIBILITY_RACE_DRIVE_ASSIST_H

#include <vector>

namespace a11y::race {

struct RaceState;
class CourseMap;
class Handedness;
struct Curve;

// Describes the track; never drives. Nothing here reads input, writes to the game, or changes what
// the kart does - a sighted player has the same information from the screen, and a blind player
// must end up with the same information and no more.
//
// Three things happen at once:
//
//  - The steering guide produces a pan value and nothing else. That value is applied to the game's
//    OWN engine sound, which already rises with speed - so the player hears one engine, theirs,
//    leaning towards where the racing line goes. Synthesising a tone here would put a second engine
//    on top of the first, and that is not what this is.
//  - The curve call is spoken once per corner per lap, far enough ahead to act on.
//  - Approach and traversal beeps mark the corner arriving and its entry, apex and exit. These are
//    brief, so they may pan on their own without fighting the engine.
//
// Every threshold that could be a distance is a time instead - seconds of lead at the current
// speed. That is what the player actually needs, it adapts to speed for free, and it avoids
// depending on what a KMP coordinate unit means.
class DriveAssist {
public:
    void Reset();

    // `station` is the player's checkpoint station, resolved once per frame by the caller so the
    // map is not searched twice.
    void Tick(const RaceState& state, const CourseMap& map, const Handedness& handedness,
              int station, float dtSec);

    // Where the racing line wants the kart pointed: -1 hard left, 0 on line, +1 hard right. This is
    // the steering guide's entire output - the caller applies it to the game's engine sound.
    float SteeringPan() const { return mSmoothedPan; }

    // Temporary diagnostics: the raw bearing to the aim point, in degrees, and how far ahead the
    // guide was aiming. Remove with the telemetry once the feel is settled.
    float LastBearingDegrees() const { return mLastBearingDeg; }
    float LastReachWidths() const { return mLastReachWidths; }

private:
    void UpdateSteering(const RaceState& state, const CourseMap& map,
                        const Handedness& handedness, int station, float dtSec);
    // What the position term worked out this frame. Kept together so the diagnostic can print the
    // whole chain rather than just its answer - the last time this term shipped, the log said only
    // what it produced and not what it was made of.
    struct PositionLean {
        float pan = 0.0f;        // absolute pan, independent of the guide's strength knob
        float u = 0.0f;          // fraction of the real road the prediction spans
        float lateral = 0.0f;    // units from the line now, signed to the track's right
        float rate = 0.0f;       // units per second across the line - diagnostic only
        float predicted = 0.0f;  // units from the line at the aim point, the value that is graded
    };

    // Top Speed's position term, in ABSOLUTE pan, graded on where the kart's heading is taking it
    // rather than on where it is. Extrapolates over the ROAD's own half-width, not the pursuit term's look-ahead: that horizon
    // is a lever on the kart's yaw, and sharing it made a held drift read as most of the road.
    PositionLean PositionPan(const RaceState& state, const CourseMap& map,
                             const Handedness& handedness, float arc, float predictDistance) const;
    // One active curve drives every curve cue, as in MK64: the spoken call, the approach countdown
    // and the entry/apex/exit beeps all describe the same corner and change together.
    void UpdateCurveCues(const RaceState& state, const CourseMap& map, int station);

    float mSmoothedPan = 0.0f;
    // How fast the kart is turning, radians per second, positive toward its right - the same sign
    // every other cue uses. Differentiated from the heading and smoothed on the guide's own time
    // constant. The pursuit term subtracts what this already accounts for, which is what makes the
    // cue warn BEFORE a corner without moving its centre.
    float mYawRate = 0.0f;
    float mLastForwardX = 0.0f;
    float mLastForwardZ = 0.0f;
    bool mHaveLastForward = false;
    // The last full-lean side chosen while the aim point's bearing was still meaningful - held
    // while it sits nearly dead astern, where its side is numerical noise.
    float mLastToward = 0.0f;
    float mLastBearingDeg = 0.0f;
    float mLastReachWidths = 0.0f;
    // Which kPositionLogStep buckets the two leans last printed in, so the diagnostic follows real
    // movement of EITHER of them - bucketing on the position term alone was blind to the very
    // event it needed to show, a pursuit term swinging through zero against a steady position one.
    int mLastPositionBucket = 0;
    int mLastPursuitBucket = 0;
    // Diagnostic only: the previous frame's lateral offset, so the log can separate the kart
    // moving across the line from the line moving under the kart.
    float mLastLateralUnits = 0.0f;

    int mLastLap = -1;

    // The corner currently being described, identified by its entry station. Every counter below
    // belongs to it and resets when it changes.
    int mActiveEntry = -1;
    // The corner the countdown is leading, which is the one AHEAD and not the one being
    // described - they differ whenever the kart is still inside the previous corner.
    int mApproachEntry = -1;
    int mApproachBeeps = 0;
    bool mAnnounced = false;
    // 0 before the entry, 1 past it, 2 past the apex, 3 past the exit.
    int mPhase = 0;
    // The corner whose exit beep has already sounded, by entry station. It stays selectable for the
    // rest of its clearance margin and its entry is the furthest behind, so it wins the selection
    // back on the very next frame - which reset the counters, replayed its exit beep and re-spoke
    // the next corner every other frame. Latched here, the handoff holds. One is enough: by the
    // time the next corner finishes, this one is a whole corner behind.
    int mFinishedEntry = -1;
    // Corners already spoken inside a chained call ("left, then right"), by entry station. A
    // follower on this list keeps its traversal beeps but is not re-announced; one past the
    // kMaxChain cap is NOT on it, so it still gets a call of its own. Cleared each lap.
    std::vector<int> mChainAnnounced;
};

// Menu preview: plays a representative corner-entry beep as a one-shot.
void PlayCurveCueDemo();

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_DRIVE_ASSIST_H
