#ifndef MKW_ACCESSIBILITY_RACE_COURSE_MAP_H
#define MKW_ACCESSIBILITY_RACE_COURSE_MAP_H

#include <cstdint>
#include <vector>

#include "accessibility/race/route_graph.h"

namespace a11y::race {

struct Checkpoint;

enum class TurnSeverity { Easy, Normal, Hard, Hairpin };

// A stretch of the line bending one way. Landmarks are continuous arc positions along the lap,
// never station indices: the stations are a sampling grid, the corner is a place.
struct Curve {
    // The stations the bend spans, inclusive; the corner's identity, which survives the line
    // being moved sideways. `entry`/`length` are derived from them on the CURRENT line.
    int first = 0;
    int last = 0;
    float entry = 0.0f;   // arc where the bend begins, in [0, LapLength())
    float length = 0.0f;  // arc from entry to exit, always positive
    bool right = false;
    TurnSeverity severity = TurnSeverity::Easy;
    bool isLong = false;
    // Chained to the corner before it: the gap between them is shorter than a straight
    // (anticipation.h kStraightUnits). A follower is named in its run's phrase and never counts
    // down. A course property, so it is settled here.
    bool follower = false;
    float peakCurvature = 0.0f;  // rad per unit, unsigned
    float totalDegrees = 0.0f;
};

// The driving line and the track limits.
//
// Stations come from walking the AI route through the successor links the game computed, so the
// line is on the road by construction, then RESAMPLED at one uniform spacing - the lap's median
// corridor half-width - so that curvature, corner shape and every distance threshold mean the same
// thing on every course regardless of how densely its route was authored. Checkpoints are kept
// for the two things they are good at: the game's own progress index, and settling which side is
// "right". A course whose route never closes a lap falls back to the checkpoint midpoints for
// ordering only - the lateral position still comes from the route, or not at all.
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

    // Moves each station sideways by `shifts[i]` world units towards the track's right: the
    // racing line the edge map placed inside the real road. Only the stations move:
    // RoadOffsetAtArc measures against them too, so the guide's target and the offset's zero are
    // one line by construction - centre pan has to keep meaning "you are on the line", which is
    // the player's own definition of the cue.
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
    // Unsigned forward distance along the lap, in [0, LapLength()). The signed measures fold at
    // half a lap, so anything measured against a corner that can be longer than that - Baby Park's
    // single continuous curve, and any oval emitted as one - must use these instead.
    float ArcForwardTo(float fromArc, int toStation) const;
    float ArcForwardFrom(int fromStation, float toArc) const;
    // The same, between two continuous arc positions.
    float ArcBetween(float fromArc, float toArc) const { return WrapForward(toArc - fromArc); }
    // An arc difference folded into [0, LapLength()).
    float WrapForward(float d) const;

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

    // Uniform after resampling: the lap's median corridor half-width.
    float MeanSpacing() const { return mMeanSpacing; }

    int NearestStation(float x, float z, int hint) const;

    // Where the kart sits across the ROAD: -1 at the left edge, 0 in the middle, +1 at the right.
    // False when the route did not read, which means stay silent rather than guess - a position
    // cue that says "centred" while the kart is on the grass is worse than no cue at all.
    // `closestX/Z`, when asked for, are the point on the line the offset was measured to, and
    // `halfWidth` the corridor half-width it was normalized by.
    //
    // Measured against the LAP at `arc` - the very line the steering guide aims at, so "centre
    // pan means you are on the line" stays true by construction. The graph-wide search remains
    // the fallback for a map that never closed a lap.
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

    // The same right-hand vector at a continuous arc position, off the blended forward there.
    void RightVectorAtArc(float arc, float& x, float& z) const;

    // How far a sideways walk from this station stays on this station's own stretch of course,
    // before the ground it reaches belongs to another one. The route's own medial axis, so it
    // needs no constant. See the definition for why a lateral sweep needs it at all.
    float LateralReach(int station, bool right) const;

    // +1 when "right" is (forward.z, -forward.x), -1 when it is the other perpendicular - the
    // world's chirality, settled from the checkpoint data. The kart lives in the same world, so
    // its own right is the same perpendicular; sharing this is what keeps every signed cue on one
    // convention with nothing observed while driving.
    float RightPerpSign() const { return mRightPerpSign; }

    // Signed curvature of the line at a station, positive to the right, rad per unit of travel,
    // smoothed over one road width. Zero until the map is built.
    float CurvatureAt(int i) const;

    // ---- Corners -------------------------------------------------------------------------------
    //
    // Corner shape and run membership are geometry alone: fixed radii and fixed distances, the
    // same on every course and at every engine class.
    // Bumps every time the corner list is rebuilt, so a consumer keyed on corner indices can
    // drop its state instead of describing corners that no longer exist.
    unsigned CurveGeneration() const { return mCurveGeneration; }

    const std::vector<Curve>& Curves() const { return mCurves; }
    float CurveApex(const Curve& c) const { return WrapForward(c.entry + c.length * 0.5f); }
    float CurveExit(const Curve& c) const { return WrapForward(c.entry + c.length); }
    // The corner whose span contains this arc, or nullptr on a straight.
    const Curve* CurveContaining(float arc) const;
    // The corner after / before this one along the lap (wraps; a lap with one corner returns it).
    const Curve* CurveAfter(const Curve& from) const;
    const Curve* CurveBefore(const Curve& from) const;
    // Straight between one corner's exit and the next corner's entry, along the lap.
    float GapAfter(const Curve& from, const Curve& next) const;

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
    // The blended forward inside an already-resolved segment, so a caller that has the segment
    // does not pay for resolving it again. Left zero when the two ends nearly cancel.
    void ForwardInSegment(int station, float t, float& x, float& z) const;
    bool BuildRouteStations();
    void ResampleUniform();
    void BuildCheckpointStations(const std::vector<Checkpoint>& checkpoints);
    void BuildDerived();
    void BuildCheckpointMap(const std::vector<Checkpoint>& checkpoints);
    // Curvature per station, then the corners (course_curves.cpp). Both read the ROAD - the
    // authored route - never the racing line: the line crosses the road between corners and
    // that S is a real bend of the line but not a corner of the course.
    void BuildCurvature();
    void BuildCurves();
    // Re-derives each corner's arc landmarks from the current stations (after a road shift).
    void RefreshCurveArcs();
    float SignedTurnAt(int i) const;
    // Temporary diagnostic dump of the segmentation result.
    void LogCurveMap() const;

    RouteGraph mGraph;
    std::vector<Station> mPoints;
    std::vector<float> mArc;
    std::vector<float> mCurvature;  // signed, positive to the right, rad per unit, smoothed
    std::vector<float> mRawTurn;    // signed heading change at each station, radians
    std::vector<int> mStationForCheckpoint;
    std::vector<Curve> mCurves;
    unsigned mCurveGeneration = 0;
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
