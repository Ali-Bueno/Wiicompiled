#ifndef MKW_ACCESSIBILITY_RACE_DRIVE_ASSIST_H
#define MKW_ACCESSIBILITY_RACE_DRIVE_ASSIST_H

#include <vector>

namespace a11y::race {

struct RaceState;
class CourseMap;
class Handedness;
struct Curve;

// Describes the track; never drives. Nothing here reads input, writes to the game, or changes what
// the kart does.
//
//  - The steering guide produces one pan value, applied to the game's own engine note: the
//    bearing to an aim point on the racing line a reaction time ahead (Forza's guide).
//  - The curve call is spoken once per corner per lap, far enough ahead to act on.
//  - Approach and traversal beeps mark the corner arriving and its entry, apex and exit.
//
// Every lead is a time at the current speed, never a distance, so 50cc and 500cc get the same
// seconds to react.
class DriveAssist {
public:
    void Reset();

    // `station` is the player's checkpoint station, resolved once per frame by the caller.
    void Tick(const RaceState& state, const CourseMap& map, const Handedness& handedness,
              int station, float dtSec);

    // -1 engine hard left, 0 on line, +1 hard right. The engine sits on the side the kart is
    // heading for - the side to steer away from.
    float SteeringPan() const { return mSmoothedPan; }

    // Diagnostics for the telemetry line: the bearing to the aim point in degrees, positive with
    // the point on the kart's right, and how far ahead along the line it was taken.
    float LastBearingDeg() const { return mLastBearingDeg; }
    float LastHorizonUnits() const { return mLastHorizonUnits; }

private:
    void UpdateSteering(const RaceState& state, const CourseMap& map,
                        const Handedness& handedness, int station, float dtSec);
    void UpdateCurveCues(const RaceState& state, const CourseMap& map, int station);

    float mSmoothedPan = 0.0f;
    float mLastBearingDeg = 0.0f;
    float mLastHorizonUnits = 0.0f;
    int mLastLogBucket = 0;

    int mLastLap = -1;

    // The corner currently being described, identified by its entry station. Every counter below
    // belongs to it and resets when it changes.
    int mActiveEntry = -1;
    // The corner the countdown is leading - the one AHEAD, which differs from the one being
    // described while the kart is still inside the previous corner.
    int mApproachEntry = -1;
    int mApproachBeeps = 0;
    bool mAnnounced = false;
    // 0 before the entry, 1 past it, 2 past the apex, 3 past the exit.
    int mPhase = 0;
    // The corner whose exit beep has already sounded, so it cannot take the focus back.
    int mFinishedEntry = -1;
    // The chain gap frozen at the last run's call (units), shared by its phrase and countdown veto.
    float mChainGap = 0.0f;
    // Corners already spoken inside a chained call ("left, then right"). Cleared each lap.
    std::vector<int> mChainAnnounced;
};

// Menu preview: plays a representative corner-entry beep as a one-shot.
void PlayCurveCueDemo();

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_DRIVE_ASSIST_H
