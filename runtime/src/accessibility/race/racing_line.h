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
// nothing moves. Each solve is a banded factorisation, linear in the station count, so a caller
// can afford several iterations per frame (StepFor).
class RacingLine {
public:
    // `settledUnits` is the move below which a station counts as placed.
    void Begin(const CourseMap& map, std::vector<LineBand> bands, float settledUnits);
    // One active-set iteration. True once settled or out of iterations; read Shifts() then.
    bool Step();
    // Iterates until settled or the budget is spent, always at least once. Same return as Step.
    bool StepFor(double budgetMs);
    bool Done() const { return mDone; }
    // Towards the track's right, in station order, all inside their bands.
    const std::vector<float>& Shifts() const { return mShift; }
    int Iterations() const { return mIterations; }
    int Pinned() const { return mPinned; }

private:
    bool SolveFree();
    bool FactorBand(int rows);
    void BandSolve(int rows, double* v) const;
    double HAt(int i, int j) const;
    double FreeMove(int i) const;
    void LogKkt();

    std::vector<LineBand> mBands;
    std::vector<float> mShift;
    // Which bound each station is pinned to: 0 free, -1 low, +1 high.
    std::vector<int> mPin;
    // Quadratic model of the bending energy about the authored line: J(a) = a'Ha/2 + g'a. H is
    // held as a periodic band - row i holds H(i, i+d) for d in [-2,+2] - because a second
    // difference couples a station to nothing further than two stations away.
    std::vector<double> mHBand;
    std::vector<double> mG;
    // Scratch for SolveFree, sized once in Begin: a solve runs every iteration and these were a
    // fresh heap allocation each time.
    std::vector<int> mFree;
    std::vector<double> mRhs;
    // Lower banded Cholesky of the leading free block, three offsets per row.
    std::vector<double> mBandL;
    // The trailing free columns, column-major: the raw block and its banded solve.
    std::vector<double> mBorder;
    std::vector<double> mBorderSolved;
    float mSettledUnits = 0.0f;
    int mIterations = 0;
    int mMaxIterations = 0;
    int mPinned = 0;
    bool mDone = false;
    bool mLoggedKkt = false;
};

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_RACING_LINE_H
