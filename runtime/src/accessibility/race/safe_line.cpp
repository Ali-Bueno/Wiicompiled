#include "accessibility/race/safe_line.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include "accessibility/race/course_map.h"
#include "accessibility/race/kcl_road.h"
namespace a11y::race {
// Keep the game's authored CPU route wherever it is safe. Only a point outside the measured safe
// band moves, by the smallest amount that puts it back inside. The propagated bounds keep those
// repairs gradual, so a missing KCL sample cannot insert a steering step into the route. Returns
// how many measured bands had to be released as not this road's.
int RepairSafeLine(const CourseMap& map, std::vector<StationEdges>& edges,
                   float kartHalfWidth, float warningDistance) {
    const int count = static_cast<int>(edges.size());
    int constrained = 0;
    if (count < 3) {
        return 0;
    }

    // Half the game's 0.30-radian CPU corner threshold limits the extra turn introduced by a
    // repair; the route itself retains all of its authored cornering.
    constexpr float kRepairTurnRad = 0.30f * 0.5f;
    const float slope = std::tan(kRepairTurnRad);
    std::vector<float> maxStep(static_cast<std::size_t>(count), 0.0f);
    for (int i = 0; i < count; ++i) {
        maxStep[static_cast<std::size_t>(i)] = map.ArcForward(i - 1, i) * slope;
    }

    auto bounded = [](const StationEdges& edge, bool right) {
        const EdgeKind kind = edge.Kind(right);
        return kind != EdgeKind::Unknown && kind != EdgeKind::Open;
    };

    std::vector<float> lo(static_cast<std::size_t>(count), 0.0f);
    std::vector<float> hi(static_cast<std::size_t>(count), 0.0f);
    for (int i = 0; i < count; ++i) {
        const StationEdges& e = edges[static_cast<std::size_t>(i)];
        const bool haveLeft = bounded(e, false);
        const bool haveRight = bounded(e, true);
        float localHalfWidth = 0.0f;
        if (haveLeft && haveRight) {
            localHalfWidth = (e.leftDistance + e.rightDistance) * 0.5f;
        } else if (haveLeft) {
            localHalfWidth = e.leftDistance;
        } else if (haveRight) {
            localHalfWidth = e.rightDistance;
        }
        if (!haveLeft || !haveRight || !(localHalfWidth > 0.0f)) {
            // No complete corridor: nothing measured to keep the point inside, so it only follows
            // its neighbours. Pinning it to the authored point instead folded Moo Moo's line 52
            // degrees where a 1025-unit repair met five unmeasured stations (2026-09-09).
            lo[i] = -std::numeric_limits<float>::infinity();
            hi[i] = std::numeric_limits<float>::infinity();
            continue;
        }
        // Reserve the warning margin and a full kart width where the local road permits it.
        const float clear = std::min(localHalfWidth,
            std::max({warningDistance, 2.0f * kartHalfWidth, kKclLateralStepUnits}));
        lo[i] = -e.leftDistance + clear;
        hi[i] = e.rightDistance - clear;
    }

    const auto localLo = lo;
    const auto localHi = hi;
    constexpr float kInf = std::numeric_limits<float>::infinity();

    // Carry every hard bound both ways around the lap. Each pass can move it across one more
    // station, and the monotone bounds reach a fixed point in at most one lap.
    auto propagate = [&]() {
        for (int pass = 0; pass < count; ++pass) {
            bool changed = false;
            for (int i = 0; i < count; ++i) {
                const std::size_t here = static_cast<std::size_t>(i);
                const std::size_t back = static_cast<std::size_t>((i + count - 1) % count);
                const float step = maxStep[here];
                if (lo[back] - step > lo[here]) { lo[here] = lo[back] - step; changed = true; }
                if (hi[back] + step < hi[here]) { hi[here] = hi[back] + step; changed = true; }
            }
            for (int i = count - 1; i >= 0; --i) {
                const std::size_t here = static_cast<std::size_t>(i);
                const std::size_t ahead = static_cast<std::size_t>((i + 1) % count);
                const float step = maxStep[ahead];
                if (lo[ahead] - step > lo[here]) { lo[here] = lo[ahead] - step; changed = true; }
                if (hi[ahead] + step < hi[here]) { hi[here] = hi[ahead] + step; changed = true; }
            }
            if (!changed) {
                break;
            }
        }
    };

    // A band the slope limit cannot join to its neighbours' is not this road's cross-section -
    // another road alongside, a paved infield, a side that read wrong - and folding the line to
    // its midpoint put a 170-degree kink into Toad's Factory (road corpus, 2026-09-09). Such a
    // band is released and the station follows its neighbours like an unmeasured one. Two bands
    // in conflict cannot say which is wrong; the band in conflict with the most of the bands
    // around it is the odd one out, so that one goes first, until no pair conflicts.
    std::vector<std::uint8_t> released(static_cast<std::size_t>(count), 0);
    auto measured = [&](int i) {
        return released[static_cast<std::size_t>(i)] == 0 &&
               std::isfinite(localLo[static_cast<std::size_t>(i)]);
    };
    float meanStep = 0.0f, lowest = kInf, highest = -kInf;
    for (int i = 0; i < count; ++i) {
        meanStep += maxStep[static_cast<std::size_t>(i)] / static_cast<float>(count);
        if (measured(i)) {
            lowest = std::min(lowest, localLo[static_cast<std::size_t>(i)]);
            highest = std::max(highest, localHi[static_cast<std::size_t>(i)]);
        }
    }
    // Only bands closer than the widest spread of bands over the slope limit can conflict at all.
    int window = 0;
    if (meanStep > 0.0f && highest > lowest) {
        window = std::min(count / 2, static_cast<int>(std::ceil((highest - lowest) / meanStep)));
    }
    for (int round = 0; round < count; ++round) {
        int worst = -1, worstConflicts = 0;
        float worstOver = 0.0f;
        for (int i = 0; i < count; ++i) {
            if (!measured(i)) continue;
            int conflicts = 0;
            float over = 0.0f;
            for (int k = 1; k <= window; ++k) {
                for (int j : {(i + k) % count, (i - k + count) % count}) {
                    if (j == i || !measured(j)) continue;
                    const float allowed = meanStep * static_cast<float>(k);
                    const float gap = std::max(localLo[i] - (localHi[j] + allowed),
                                               localLo[j] - (localHi[i] + allowed));
                    if (gap > 0.0f) { ++conflicts; over = std::max(over, gap); }
                }
            }
            if (conflicts > worstConflicts || (conflicts == worstConflicts && conflicts > 0 && over > worstOver)) {
                worst = i; worstConflicts = conflicts; worstOver = over;
            }
        }
        if (worst < 0) break;
        released[static_cast<std::size_t>(worst)] = 1;
        ++constrained;
    }

    for (int i = 0; i < count; ++i) {
        lo[i] = released[i] ? -kInf : localLo[i];
        hi[i] = released[i] ? kInf : localHi[i];
    }
    propagate();

    for (int i = 0; i < count; ++i) {
        const float low = lo[static_cast<std::size_t>(i)];
        const float high = hi[static_cast<std::size_t>(i)];
        const float shift = low > high ? (low + high) * 0.5f : std::max(low, std::min(high, 0.0f));
        if (std::isfinite(shift)) {
            edges[static_cast<std::size_t>(i)].shift = shift;
        }
    }
    return constrained;
}

}  // namespace a11y::race
