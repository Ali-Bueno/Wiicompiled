#ifndef MKW_ACCESSIBILITY_RACE_DRIVE_ASSIST_H
#define MKW_ACCESSIBILITY_RACE_DRIVE_ASSIST_H

#include <vector>

namespace a11y::race {

struct RaceState;
class CourseMap;
class Handedness;
struct Curve;

// Forza's steering guide: the engine leans by the bearing from the kart to an aim point on the safe
// line one anticipation time ahead, and nothing else. Spoken calls and countdowns announce corners.
// Play-tested 40-percent maximum engine pan, fixed across vehicles (2026-08-27 to 2026-09-07).
inline constexpr float kGuideMaxPan = 0.4f;

class DriveAssist {
public:
    void Reset();

    // `station` is the player's checkpoint station, resolved once per frame by the caller.
    void Tick(const RaceState& state, const CourseMap& map, const Handedness& handedness,
              int station, float dtSec);

    // The engine sounds on the side the kart is heading for, relative to the line.
    float SteeringPan() const { return mPan; }
    // Diagnostics: the guide's own input, degrees to the aim point (positive on the kart's right)
    // and how far along the line that point sits.
    float LastBearingDeg() const { return mLastBearingDeg; }
    float LastHorizonUnits() const { return mLastHorizonUnits; }

private:
    // Everything one corner has said this pass, keyed by its index in the course map. Cleared when
    // the corner falls out of play (passed, or the kart teleported), so an oval re-arms by itself.
    struct CornerCues {
        bool called = false;
        int stagesDone = 0;  // countdown stages fired or forfeited
        int phase = 0;       // 0 before the entry, 1 past it, 2 past the apex, 3 past the exit
    };

    void UpdateSteering(const RaceState& state, const CourseMap& map,
                        const Handedness& handedness, int station, float dtSec);
    void UpdateCurveCues(const RaceState& state, const CourseMap& map, int station, float dtSec);
    void RebindCurves(const CourseMap& map);

    float mPan = 0.0f;
    float mLastBearingDeg = 0.0f;
    float mLastHorizonUnits = 0.0f;
    // Facing away from the aim point the bearing sits on the +-180 degree seam; the ear it chose
    // is held until the bearing has clearly come back, so a spin does not flicker between ears.
    bool mAstern = false;
    float mAsternPanSign = 0.0f;

    unsigned mCurveGeneration = 0;
    std::vector<CornerCues> mCues;
    float mLastArc = -1.0f;
    float mLastX = 0.0f, mLastZ = 0.0f;
    std::vector<float> mPendingLandmarks;
    std::vector<bool> mPendingRight;
    float mLandmarkBusySec = 0.0f;
};

// Menu preview: plays a representative corner-entry beep.
void PlayCurveCueDemo();

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_DRIVE_ASSIST_H
