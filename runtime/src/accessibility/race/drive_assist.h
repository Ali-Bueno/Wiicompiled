#ifndef MKW_ACCESSIBILITY_RACE_DRIVE_ASSIST_H
#define MKW_ACCESSIBILITY_RACE_DRIVE_ASSIST_H

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
    // One active curve drives every curve cue, as in MK64: the spoken call, the approach countdown
    // and the entry/apex/exit beeps all describe the same corner and change together.
    void UpdateCurveCues(const RaceState& state, const CourseMap& map, int station);

    float mSmoothedPan = 0.0f;
    // The last full-lean side chosen while the aim point's bearing was still meaningful - held
    // while it sits nearly dead astern, where its side is numerical noise.
    float mLastToward = 0.0f;
    float mLastBearingDeg = 0.0f;
    float mLastReachWidths = 0.0f;

    int mLastLap = -1;

    // The corner currently being described, identified by its entry station. Every counter below
    // belongs to it and resets when it changes.
    int mActiveEntry = -1;
    int mApproachBeeps = 0;
    bool mAnnounced = false;
    // 0 before the entry, 1 past it, 2 past the apex, 3 past the exit.
    int mPhase = 0;
};

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_DRIVE_ASSIST_H
