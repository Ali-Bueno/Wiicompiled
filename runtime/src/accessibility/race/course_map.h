#ifndef MKW_ACCESSIBILITY_RACE_COURSE_MAP_H
#define MKW_ACCESSIBILITY_RACE_COURSE_MAP_H

#include <cstdint>
#include <vector>

#include "accessibility/race/route_graph.h"

namespace a11y::race {

struct Checkpoint;

enum class TurnSeverity { Easy, Normal, Hard, Hairpin };

// A run of consecutive stations turning the same way. Landmarks are station indices.
struct Curve {
    int entry = 0;
    int apex = 0;
    int exit = 0;
    bool right = false;
    TurnSeverity severity = TurnSeverity::Easy;
    bool isLong = false;
    // The corner's peak tightness expressed against the game's own "this is a corner" threshold:
    // 1.0 is exactly that corner, a hairpin is a little over 3. The severity enum is the same
    // measurement quantised for speech; this keeps it continuous for the steering guide, which
    // deepens its lean with it and would step audibly on four levels.
    float intensity = 0.0f;
};

// The driving line and the track limits.
//
// Stations come from walking the AI route through the successor links the game computed, so the
// line is on the road by construction. Checkpoints are kept for the two things they are good at:
// the game's own progress index, and settling which side is "right". A course whose route never
// closes a lap falls back to the checkpoint midpoints for ordering only - the lateral position
// still comes from the route, or not at all.
class CourseMap {
public:
    // True when the stations came from the route geometry, it closed a lap, AND the lap length
    // agrees with the game's own; false on the checkpoint-midpoint fallback (map may still be
    // Loaded), so a caller with another route source available knows to try it.
    bool Build(std::vector<RoutePoint> route, std::uint8_t startPoint,
               const std::vector<Checkpoint>& checkpoints);
    void Clear();
    bool Loaded() const { return mPoints.size() >= kMinStations; }

    // Whether the stations are real route geometry. The checkpoint-midpoint fallback carries
    // progress and corner shape but NEVER position - anything that aims at the line (the steering
    // guide) must gate on this, not on Loaded().
    bool RouteBased() const { return mRouteBased; }

    int StationCount() const { return static_cast<int>(mPoints.size()); }

    // Moves each station sideways by `shifts[i]` world units towards the track's right, to stand
    // the line on the asphalt where the course authored it off the road. Only the stations move:
    // RoadOffsetAtArc measures against them too, so the guide's target and the offset's zero are
    // one line by construction - centre pan has to keep meaning "you are on the line", which is
    // the player's own definition of the cue.
    //
    // A repair, not a re-authoring: a station the route already put on the road gets a zero shift
    // and does not move. Smoothed across neighbours first, because a step in the line reads as a
    // corner to the curvature pass and would have the mod calling turns that are not there.
    //
    // Only on a route-based map, and only once - the caller gates on RoadShifted().
    bool ApplyRoadShift(const std::vector<float>& shifts);
    bool RoadShifted() const { return mRoadShifted; }

    // The station the game's checkpoint index corresponds to, or -1 when out of range.
    int StationForCheckpoint(int checkpoint) const;

    void Centre(int i, float& x, float& z) const;

    // The authored height of the route point this station came from. Never smoothed and never used
    // for geometry - it is the height a vertical collision probe has to start from. Zero on the
    // checkpoint fallback, which is why anything probing gates on RouteBased().
    float Height(int i) const;

    // The game's own drivable corridor at this station.
    float HalfWidth(int i) const;

    float ArcAt(int i) const;
    float LapLength() const { return mLapLength; }
    float ArcForward(int from, int to) const;
    // Signed distance along the lap, negative when the target is behind.
    float ArcSigned(int from, int to) const;
    // Signed lap distance from a continuous arc position to a station, negative when behind.
    float ArcSignedTo(float fromArc, int toStation) const;

    // Continuous arc position of a world point, projected onto the lap polyline within a window
    // of stations either side of the hint (the checkpoint-mapped station). The game's checkpoint
    // index is right through crossovers but only checkpoint-coarse; this refines it so distance
    // cues do not jump by a whole checkpoint at a time.
    float ArcOfPosition(float x, float z, int hintStation) const;

    // The station segment a continuous arc position falls in, and how far along it (0..1), so a
    // caller with its own per-station table can blend it the way this class blends its own.
    bool SegmentAtArc(float arc, int& station, float& t) const;

    // The line's direction at a continuous arc position, blended between the neighbouring
    // stations' forwards so a look-ahead sample moves smoothly instead of stepping per segment.
    void ForwardAtArc(float arc, float& x, float& z) const;

    // The point ON the line at a continuous arc position, interpolated within its segment - the
    // pursuit target. False when no line is loaded.
    bool PointAtArc(float arc, float& x, float& z) const;

    float MeanSpacing() const { return mMeanSpacing; }

    int NearestStation(float x, float z, int hint) const;

    // Where the kart sits across the ROAD: -1 at the left edge, 0 in the middle, +1 at the right.
    // False when the route did not read, which means stay silent rather than guess - a position
    // cue that says "centred" while the kart is on the grass is worse than no cue at all.
    // `closestX/Z`, when asked for, are the point on the line the offset was measured to, and
    // `halfWidth` the corridor half-width it was normalized by.
    //
    // Measured against the LAP at `arc` - the very line the steering guide aims at. It used to be
    // measured against the nearest segment of the whole route graph, branches included, and on a
    // branching course that alternated between two neighbouring branches frame by frame: a logged
    // Toad's Factory race jumped the offset by 1958, -1768 and 1251 units in single frames at a
    // speed of 63 units per frame, each with the pursuit term barely moving. The two halves of the
    // pan were describing two different lines. One reference keeps "centre pan means you are on
    // the line" true, which is the player's own definition of the cue.
    //
    // The graph-wide search remains the fallback for a map that never closed a lap, where there
    // are no route stations to measure against.
    bool RoadOffsetAtArc(float arc, float x, float y, float z, float& out,
                         float* closestXOut = nullptr, float* closestZOut = nullptr,
                         float* halfWidthOut = nullptr) const;

    // The lap's median corridor half-width - the stable length scale for "how far across the
    // road", where the per-station corridor is the CPU's lane discipline and can narrow without
    // the road narrowing.
    float MedianHalfWidth() const { return mMedianHalfWidth; }

    // Heading of the line at a station, as a unit vector in the horizontal plane.
    void Forward(int i, float& x, float& z) const;

    // The station's right-hand vector. Which perpendicular that is was settled once at build time
    // from a checkpoint's own left-to-right vector, so it is derived rather than assumed.
    void RightVector(int i, float& x, float& z) const;

    // The same right-hand vector at a continuous arc position, off the blended forward there. It
    // lives beside RightVector so the perpendicular is derived in one place: a caller that needs
    // "across the line" at the kart's own arc must not rebuild it from ForwardAtArc itself.
    void RightVectorAtArc(float arc, float& x, float& z) const;

    // +1 when "right" is (forward.z, -forward.x), -1 when it is the other perpendicular - the
    // world's chirality, settled from the checkpoint data. The kart lives in the same world, so
    // its own right is the same perpendicular; sharing this is what keeps every signed cue on one
    // convention with nothing observed while driving.
    float RightPerpSign() const { return mRightPerpSign; }

    // The first station at least this far forward along the lap.
    int StationAhead(int station, float distance) const;

    // A point this far forward along the line, interpolated between stations rather than snapped to
    // one. Snapping makes the target jump every time a station is passed, which a steering cue
    // reads as a lurch.
    void PointAhead(int station, float distance, float& x, float& z) const;

    const std::vector<Curve>& Curves() const { return mCurves; }

    const Curve* NextCurve(int station) const;

    // The corner the cues should currently be describing: the one whose entry is nearest ahead,
    // except that a corner the kart is already inside stays selected until its exit is `clearance`
    // behind. Without that hold the choice flips at the exit and the next corner's countdown starts
    // while the player is still in this one.
    const Curve* ActiveCurve(int station, float clearance) const;

    // As ActiveCurve, measured from a continuous arc position instead of a station index.
    const Curve* ActiveCurveAt(float fromArc, float clearance) const;

    // The curve this station lies inside, or nullptr on a straight.
    const Curve* CurveAt(int station) const;

    // True when the corner before this one ends less than `gap` before it starts, so there is no
    // real straight between them and they belong to the same call.
    bool IsChainFollower(const Curve& curve, float gap) const;

    float CurveProgress(const Curve& curve, int station) const;
    float CurveLength(const Curve& curve) const;

private:
    // Below this a line is not a lap and every query would be meaningless.
    static constexpr int kMinStations = 4;

    struct Station {
        float x = 0.0f, z = 0.0f;
        float halfWidth = 0.0f;
        // Authored, never smoothed: only a collision probe reads it.
        float y = 0.0f;
    };

    int Wrap(int i) const;
    // The station segment a continuous arc position falls in, and how far along it (0..1).
    bool SegmentForArc(float arc, int& station, float& t) const;
    bool BuildRouteStations();
    void BuildCheckpointStations(const std::vector<Checkpoint>& checkpoints);
    void BuildDerived();
    void BuildCheckpointMap(const std::vector<Checkpoint>& checkpoints);
    void BuildCurves();
    void EmitCurve(int entry, int end);
    float SignedTurnAt(int i) const;
    float TightnessAt(int i) const;
    float SegmentHeading(int i) const;
    // Temporary diagnostic dump of the segmentation result.
    void LogCurveMap() const;

    RouteGraph mGraph;
    std::vector<Station> mPoints;
    std::vector<float> mArc;
    std::vector<float> mTurn;  // signed turn per station, positive to the right
    std::vector<int> mStationForCheckpoint;
    std::vector<Curve> mCurves;
    float mLapLength = 0.0f;
    float mMeanSpacing = 0.0f;
    float mMedianHalfWidth = 0.0f;
    bool mRouteBased = false;
    bool mRoadShifted = false;
    // +1 when the right-hand vector is (forward.z, -forward.x), -1 when it is the other one.
    float mRightPerpSign = 1.0f;
    // How far ArcOfPosition searches either side of its hint, in stations - one more than the
    // widest station gap between consecutive checkpoints, so the refinement always spans the
    // ground the coarse checkpoint index can lag by.
    int mHintWindow = 2;
};

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_COURSE_MAP_H
