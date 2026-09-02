#ifndef MKW_ACCESSIBILITY_RACE_RACING_LINE_H
#define MKW_ACCESSIBILITY_RACE_RACING_LINE_H

#include <vector>

namespace a11y::race {

class CourseMap;

// Where one station may stand: bounds on its move towards the track's right, in world units.
// `lo > hi` marks a station the road cannot fit, which is placed midway and reported.
struct LineBand {
    float lo = 0.0f;
    float hi = 0.0f;
};

// The line of least curvature the bands allow: the racing line. Wide into a corner, at the inside
// through it, wide out of it, straight across a gentle bend - hugging an edge only where that
// straightens the path, exactly as a sighted racer's line does. Least curvature is the line that
// asks the least of the steering, so it is also the one the guide can hold most calmly.
//
// A bound-constrained least-squares problem solved by an active set: the unconstrained line is
// solved exactly, stations that leave their band are pinned to it, and the rest re-solved until
// nothing moves. One step per frame, because each solve is a dense factorisation.
class RacingLine {
public:
    // `settledUnits` is the move below which a station counts as placed.
    void Begin(const CourseMap& map, std::vector<LineBand> bands, float settledUnits);
    // One active-set iteration. True once settled or out of iterations; read Shifts() then.
    bool Step();
    bool Done() const { return mDone; }
    // Towards the track's right, in station order, all inside their bands.
    const std::vector<float>& Shifts() const { return mShift; }
    int Iterations() const { return mIterations; }
    int Pinned() const { return mPinned; }

private:
    bool SolveFree();

    std::vector<LineBand> mBands;
    std::vector<float> mShift;
    // Which bound each station is pinned to: 0 free, -1 low, +1 high.
    std::vector<int> mPin;
    // Quadratic model of the bending energy about the authored line: J(a) = a'Ha/2 + g'a.
    std::vector<double> mH;
    std::vector<double> mG;
    float mSettledUnits = 0.0f;
    int mIterations = 0;
    int mPinned = 0;
    bool mDone = false;
};

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_RACING_LINE_H
