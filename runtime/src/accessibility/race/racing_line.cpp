#include "accessibility/race/racing_line.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>

#include "accessibility/a11y_log.h"
#include "accessibility/race/course_map.h"

namespace a11y::race {
namespace {

int WrapIndex(int i, int n) { return (i % n + n) % n; }

// k_j is a second difference, so it touches stations j-1..j+1 and H = D'WD couples a station to
// nothing further than two stations away, the wrap included: a periodic pentadiagonal.
constexpr int kBandHalf = 2;
constexpr int kBandWidth = 2 * kBandHalf + 1;
// A lower banded Cholesky keeps the diagonal plus kBandHalf sub-diagonals per row.
constexpr int kBandRow = kBandHalf + 1;
// Every wrap-around coupling touches one of the last two stations, and those are the last free
// unknowns whenever they are free at all, so holding kBandHalf of them back as a dense border
// leaves a strictly banded block behind.
constexpr int kBorderCols = kBandHalf;

// Station separation folded to the shorter way round the loop, so that on a very short course the
// two ends of the band cannot name the same station twice.
int WrapDelta(int i, int j, int n) {
    int d = (j - i) % n;
    if (d < 0) {
        d += n;
    }
    return d > n / 2 ? d - n : d;
}

}  // namespace

double RacingLine::HAt(int i, int j) const {
    const int n = static_cast<int>(mShift.size());
    const int d = WrapDelta(i, j, n);
    if (d < -kBandHalf || d > kBandHalf) {
        return 0.0;
    }
    return mHBand[static_cast<std::size_t>(i) * kBandWidth + (d + kBandHalf)];
}

void RacingLine::Begin(const CourseMap& map, std::vector<LineBand> bands, float settledUnits) {
    const int n = map.StationCount();
    mBands = std::move(bands);
    mSettledUnits = settledUnits;
    mShift.assign(static_cast<std::size_t>(n), 0.0f);
    mPin.assign(static_cast<std::size_t>(n), 0);
    mIterations = 0;
    mPinned = 0;
    mLoggedKkt = false;
    // Active-set bound: with one bounded degree of freedom per station, every pass pins a station
    // or releases one, and each station can be pinned once and released once before the working set
    // would repeat - 2n passes, plus the last one that changes nothing and calls it settled.
    mMaxIterations = 2 * n + 1;
    mDone = n < 4 || static_cast<int>(mBands.size()) != n;
    if (mDone) {
        return;
    }

    // Authored centres, right vectors and segment lengths h[i] = |c_i - c_i-1|. Two stations closer
    // than the settle tolerance are one point; the slope between them would otherwise dominate.
    std::vector<double> cx(n), cz(n), nx(n), nz(n), h(n);
    for (int i = 0; i < n; ++i) {
        float x = 0.0f, z = 0.0f;
        map.Centre(i, x, z);
        cx[i] = x;
        cz[i] = z;
        map.RightVector(i, x, z);
        nx[i] = x;
        nz[i] = z;
        h[i] = std::max<double>(map.ArcForward(WrapIndex(i - 1, n), i), settledUnits);
    }

    // Bending energy of the closed polyline, discretised as the integral of squared curvature:
    // k_j is the change of slope across station j, weighted by that station's share of the arc.
    // J(a) = sum_j W_j |k_j0 + sum_m a_m d_jm|^2, with d_jm the derivative of k_j by station m's
    // shift, non-zero only for m in {j-1, j, j+1}. Assembled into the band of H and into g.
    mHBand.assign(static_cast<std::size_t>(n) * kBandWidth, 0.0);
    mG.assign(static_cast<std::size_t>(n), 0.0);
    // The free set is at most every station, so this is the one sizing SolveFree ever needs.
    mFree.reserve(static_cast<std::size_t>(n));
    mRhs.resize(static_cast<std::size_t>(n));
    mBandL.resize(static_cast<std::size_t>(n) * kBandRow);
    mBorder.resize(static_cast<std::size_t>(n) * kBorderCols);
    mBorderSolved.resize(static_cast<std::size_t>(n) * kBorderCols);
    for (int j = 0; j < n; ++j) {
        const int prev = WrapIndex(j - 1, n), next = WrapIndex(j + 1, n);
        const double hj = h[j], hn = h[next];
        const double w = 1.0 / (hj + hn);
        const double k0x = (cx[next] - cx[j]) / hn - (cx[j] - cx[prev]) / hj;
        const double k0z = (cz[next] - cz[j]) / hn - (cz[j] - cz[prev]) / hj;
        const int m[3] = {prev, j, next};
        const double dx[3] = {nx[prev] / hj, -nx[j] * (1.0 / hj + 1.0 / hn), nx[next] / hn};
        const double dz[3] = {nz[prev] / hj, -nz[j] * (1.0 / hj + 1.0 / hn), nz[next] / hn};
        for (int p = 0; p < 3; ++p) {
            mG[m[p]] += w * (dx[p] * k0x + dz[p] * k0z);
            for (int q = 0; q < 3; ++q) {
                const int slot = WrapDelta(m[p], m[q], n) + kBandHalf;
                mHBand[static_cast<std::size_t>(m[p]) * kBandWidth + slot] +=
                    w * (dx[p] * dx[q] + dz[p] * dz[q]);
            }
        }
    }

    // A station the road cannot fit has no choice to make.
    for (LineBand& band : mBands) {
        if (band.lo > band.hi) {
            band.lo = band.hi = (band.lo + band.hi) * 0.5f;
        }
    }
}

// In-place lower banded Cholesky of the leading free block. False when it is not positive definite,
// which the bending energy of a closed loop is unless the geometry is degenerate.
bool RacingLine::FactorBand(int rows) {
    double* l = mBandL.data();
    for (int r = 0; r < rows; ++r) {
        double* row = l + static_cast<std::size_t>(r) * kBandRow;
        double d = row[0];
        for (int k = 1; k <= kBandHalf; ++k) {
            d -= row[k] * row[k];
        }
        if (!(d > 0.0)) {
            return false;
        }
        d = std::sqrt(d);
        row[0] = d;
        // Only the next two rows carry a column-r entry, and only the first of them still overlaps
        // row r; both of the entries that needs were finished by the pass before this one.
        if (r + 1 < rows) {
            double* next = l + static_cast<std::size_t>(r + 1) * kBandRow;
            next[1] = (next[1] - next[2] * row[1]) / d;
        }
        if (r + 2 < rows) {
            l[static_cast<std::size_t>(r + 2) * kBandRow + 2] /= d;
        }
    }
    return true;
}

void RacingLine::BandSolve(int rows, double* v) const {
    const double* l = mBandL.data();
    for (int r = 0; r < rows; ++r) {
        const double* row = l + static_cast<std::size_t>(r) * kBandRow;
        double x = v[r];
        for (int k = 1; k <= kBandHalf; ++k) {
            if (r - k >= 0) {
                x -= row[k] * v[r - k];
            }
        }
        v[r] = x / row[0];
    }
    for (int r = rows - 1; r >= 0; --r) {
        double x = v[r];
        for (int k = 1; k <= kBandHalf; ++k) {
            if (r + k < rows) {
                x -= l[static_cast<std::size_t>(r + k) * kBandRow + k] * v[r + k];
            }
        }
        v[r] = x / l[static_cast<std::size_t>(r) * kBandRow];
    }
}

// Minimises J over the free stations with the pinned ones held. The free stations keep their order,
// so a coupling that does not wrap stays within kBandHalf of itself in reduced indexing: the block
// is banded once the wrapping ones are held back as a border, and a Schur complement onto that
// border finishes the solve in O(n * kBandHalf^2) instead of the cube a dense factorisation cost.
bool RacingLine::SolveFree() {
    const int n = static_cast<int>(mShift.size());
    mFree.clear();
    for (int i = 0; i < n; ++i) {
        if (mPin[i] == 0) {
            mFree.push_back(i);
        }
    }
    const int f = static_cast<int>(mFree.size());
    if (f == 0) {
        return true;
    }
    const int border = std::min(f, kBorderCols);
    const int rows = f - border;

    // Right-hand side -(g_F + H_FA a_A), in the scratch Begin sized.
    for (int r = 0; r < f; ++r) {
        const int i = mFree[r];
        double v = -mG[i];
        for (int d = -kBandHalf; d <= kBandHalf; ++d) {
            const int k = WrapIndex(i + d, n);
            // On a very short loop i-2 and i+2 are one station; count it once.
            if (WrapDelta(i, k, n) != d || mPin[k] == 0) {
                continue;
            }
            v -= HAt(i, k) * static_cast<double>(mShift[k]);
        }
        mRhs[r] = v;
    }
    // The banded block by offset: mBandL[r * kBandRow + k] holds H(free_r, free_r-k).
    for (int r = 0; r < rows; ++r) {
        for (int k = 0; k <= kBandHalf; ++k) {
            mBandL[static_cast<std::size_t>(r) * kBandRow + k] =
                r - k >= 0 ? HAt(mFree[r], mFree[r - k]) : 0.0;
        }
    }
    // The border columns, column-major, plus the copy the banded solve consumes.
    for (int c = 0; c < border; ++c) {
        const int j = mFree[rows + c];
        for (int r = 0; r < rows; ++r) {
            const double v = HAt(mFree[r], j);
            mBorder[static_cast<std::size_t>(c) * n + r] = v;
            mBorderSolved[static_cast<std::size_t>(c) * n + r] = v;
        }
    }
    if (rows > 0) {
        if (!FactorBand(rows)) {
            return false;
        }
        BandSolve(rows, mRhs.data());
        for (int c = 0; c < border; ++c) {
            BandSolve(rows, mBorderSolved.data() + static_cast<std::size_t>(c) * n);
        }
    }

    // The border's own system, S = C - B' A^-1 B, dense at kBorderCols square.
    double s[kBorderCols * kBorderCols] = {};
    double x[kBorderCols] = {};
    for (int c = 0; c < border; ++c) {
        const double* bc = mBorder.data() + static_cast<std::size_t>(c) * n;
        double v = mRhs[rows + c];
        for (int r = 0; r < rows; ++r) {
            v -= bc[r] * mRhs[r];
        }
        x[c] = v;
        for (int q = 0; q < border; ++q) {
            const double* yq = mBorderSolved.data() + static_cast<std::size_t>(q) * n;
            double e = HAt(mFree[rows + c], mFree[rows + q]);
            for (int r = 0; r < rows; ++r) {
                e -= bc[r] * yq[r];
            }
            s[c * kBorderCols + q] = e;
        }
    }
    for (int c = 0; c < border; ++c) {
        double d = s[c * kBorderCols + c];
        for (int k = 0; k < c; ++k) {
            d -= s[c * kBorderCols + k] * s[c * kBorderCols + k];
        }
        if (!(d > 0.0)) {
            return false;
        }
        d = std::sqrt(d);
        s[c * kBorderCols + c] = d;
        for (int r = c + 1; r < border; ++r) {
            double v = s[r * kBorderCols + c];
            for (int k = 0; k < c; ++k) {
                v -= s[r * kBorderCols + k] * s[c * kBorderCols + k];
            }
            s[r * kBorderCols + c] = v / d;
        }
    }
    for (int r = 0; r < border; ++r) {
        double v = x[r];
        for (int k = 0; k < r; ++k) {
            v -= s[r * kBorderCols + k] * x[k];
        }
        x[r] = v / s[r * kBorderCols + r];
    }
    for (int r = border - 1; r >= 0; --r) {
        double v = x[r];
        for (int k = r + 1; k < border; ++k) {
            v -= s[k * kBorderCols + r] * x[k];
        }
        x[r] = v / s[r * kBorderCols + r];
    }

    for (int r = 0; r < rows; ++r) {
        double v = mRhs[r];
        for (int c = 0; c < border; ++c) {
            v -= mBorderSolved[static_cast<std::size_t>(c) * n + r] * x[c];
        }
        mShift[mFree[r]] = static_cast<float>(v);
    }
    for (int c = 0; c < border; ++c) {
        mShift[mFree[rows + c]] = static_cast<float>(x[c]);
    }
    return true;
}

// The station's own free move: how far it would slide if only its own gradient moved it.
double RacingLine::FreeMove(int i) const {
    const int n = static_cast<int>(mShift.size());
    double grad = mG[i];
    for (int d = -kBandHalf; d <= kBandHalf; ++d) {
        const int k = WrapIndex(i + d, n);
        if (WrapDelta(i, k, n) != d) {
            continue;
        }
        grad += HAt(i, k) * static_cast<double>(mShift[k]);
    }
    const double diag = HAt(i, i);
    return diag > 0.0 ? -grad / diag : 0.0;
}

// Temporary self-check: a settled solve leaves every free station wanting to move nowhere and every
// pin held from outside, so a band solve that dropped a coupling shows up here rather than as a
// line that merely looks odd. In station units, against the settle tolerance.
void RacingLine::LogKkt() {
    if (mLoggedKkt) {
        return;
    }
    mLoggedKkt = true;
    const int n = static_cast<int>(mShift.size());
    double worstFree = 0.0, worstPinned = 0.0;
    for (int i = 0; i < n; ++i) {
        const double move = FreeMove(i);
        if (mPin[i] == 0) {
            worstFree = std::max(worstFree, std::fabs(move));
        } else {
            worstPinned = std::max(worstPinned, mPin[i] < 0 ? move : -move);
        }
    }
    RT_LOGF(RT_TAG_A11Y,
            "racing line: %d/%d iterations, KKT residual free %.4f, pinned %.4f, settle %.4f\n",
            mIterations, mMaxIterations, worstFree, worstPinned,
            static_cast<double>(mSettledUnits));
}

bool RacingLine::Step() {
    if (mDone) {
        return true;
    }
    const int n = static_cast<int>(mShift.size());
    ++mIterations;
    if (!SolveFree()) {
        std::fill(mShift.begin(), mShift.end(), 0.0f);  // degenerate: the line stays as authored
        mDone = true;
        return true;
    }

    // Pin every free station that left its band. A pinned station pulls its neighbours on the
    // next solve, which may push others out in turn - that is the iteration.
    bool changed = false;
    for (int i = 0; i < n; ++i) {
        if (mPin[i] != 0) {
            continue;
        }
        const LineBand& band = mBands[i];
        if (mShift[i] < band.lo) {
            mShift[i] = band.lo;
            mPin[i] = -1;
            changed = true;
        } else if (mShift[i] > band.hi) {
            mShift[i] = band.hi;
            mPin[i] = +1;
            changed = true;
        }
    }
    // Once everything fits, release a pinned station the energy would pull back INTO its band by
    // more than the settle tolerance - the pin is only needed while the pull is outward.
    if (!changed) {
        for (int i = 0; i < n; ++i) {
            if (mPin[i] == 0) {
                continue;
            }
            const double pull = FreeMove(i);
            const bool inward = mPin[i] < 0 ? pull > 0.0 : pull < 0.0;
            if (inward && std::fabs(pull) > mSettledUnits) {
                mPin[i] = 0;
                changed = true;
            }
        }
    }
    mPinned = 0;
    for (int i = 0; i < n; ++i) {
        mPinned += mPin[i] != 0 ? 1 : 0;
    }
    // Every station is inside its band by here - the pin pass above clamps any that left one - so
    // stopping at the cap leaves a feasible line, just not the settled one.
    if (!changed || mIterations >= mMaxIterations) {
        mDone = true;
        LogKkt();
    }
    return mDone;
}

bool RacingLine::StepFor(double budgetMs) {
    const auto started = std::chrono::steady_clock::now();
    do {
        Step();
    } while (!mDone && std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - started)
                               .count() < budgetMs);
    return mDone;
}

}  // namespace a11y::race
