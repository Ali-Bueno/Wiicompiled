#include "accessibility/race/racing_line.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "accessibility/race/course_map.h"

namespace a11y::race {
namespace {

int WrapIndex(int i, int n) { return (i % n + n) % n; }

}  // namespace

void RacingLine::Begin(const CourseMap& map, std::vector<LineBand> bands, float settledUnits) {
    const int n = map.StationCount();
    mBands = std::move(bands);
    mSettledUnits = settledUnits;
    mShift.assign(static_cast<std::size_t>(n), 0.0f);
    mPin.assign(static_cast<std::size_t>(n), 0);
    mIterations = 0;
    mPinned = 0;
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
    // shift, non-zero only for m in {j-1, j, j+1}. Assembled into H and g about a = 0.
    mH.assign(static_cast<std::size_t>(n) * n, 0.0);
    mG.assign(static_cast<std::size_t>(n), 0.0);
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
                mH[static_cast<std::size_t>(m[p]) * n + m[q]] +=
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

// Minimises J over the free stations with the pinned ones held, by Cholesky on the free block.
// False when that block is not positive definite, which the bending energy of a closed loop is
// unless the geometry is degenerate.
bool RacingLine::SolveFree() {
    const int n = static_cast<int>(mShift.size());
    std::vector<int> free;
    free.reserve(n);
    for (int i = 0; i < n; ++i) {
        if (mPin[i] == 0) {
            free.push_back(i);
        }
    }
    const int f = static_cast<int>(free.size());
    if (f == 0) {
        return true;
    }
    auto at = [](std::vector<double>& m, int stride, int r, int c) -> double& {
        return m[static_cast<std::size_t>(r) * stride + c];
    };
    // Right-hand side -(g_F + H_FA a_A) and the free block of H.
    std::vector<double> rhs(f, 0.0);
    std::vector<double> a(static_cast<std::size_t>(f) * f, 0.0);
    for (int r = 0; r < f; ++r) {
        const int i = free[r];
        double v = -mG[i];
        for (int k = 0; k < n; ++k) {
            if (mPin[k] != 0) {
                v -= at(mH, n, i, k) * mShift[k];
            }
        }
        rhs[r] = v;
        for (int c = 0; c < f; ++c) {
            at(a, f, r, c) = at(mH, n, i, free[c]);
        }
    }
    // In-place Cholesky, lower triangle.
    for (int c = 0; c < f; ++c) {
        double d = at(a, f, c, c);
        for (int k = 0; k < c; ++k) {
            d -= at(a, f, c, k) * at(a, f, c, k);
        }
        if (!(d > 0.0)) {
            return false;
        }
        d = std::sqrt(d);
        at(a, f, c, c) = d;
        for (int r = c + 1; r < f; ++r) {
            double v = at(a, f, r, c);
            for (int k = 0; k < c; ++k) {
                v -= at(a, f, r, k) * at(a, f, c, k);
            }
            at(a, f, r, c) = v / d;
        }
    }
    // Forward then back substitution.
    for (int r = 0; r < f; ++r) {
        double v = rhs[r];
        for (int k = 0; k < r; ++k) {
            v -= at(a, f, r, k) * rhs[k];
        }
        rhs[r] = v / at(a, f, r, r);
    }
    for (int r = f - 1; r >= 0; --r) {
        double v = rhs[r];
        for (int k = r + 1; k < f; ++k) {
            v -= at(a, f, k, r) * rhs[k];
        }
        rhs[r] = v / at(a, f, r, r);
    }
    for (int r = 0; r < f; ++r) {
        mShift[free[r]] = static_cast<float>(rhs[r]);
    }
    return true;
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
            double grad = mG[i];
            for (int k = 0; k < n; ++k) {
                grad += mH[static_cast<std::size_t>(i) * n + k] * mShift[k];
            }
            const double diag = mH[static_cast<std::size_t>(i) * n + i];
            const double pull = diag > 0.0 ? -grad / diag : 0.0;  // the station's own free move
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
    // The active set can only change a bounded number of times before it repeats; n iterations is
    // the practical cap, after which the last feasible line stands.
    if (!changed || mIterations >= n) {
        mDone = true;
    }
    return mDone;
}

}  // namespace a11y::race
