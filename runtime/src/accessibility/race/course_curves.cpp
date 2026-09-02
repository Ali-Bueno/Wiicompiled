#include "accessibility/race/course_map.h"

#include <algorithm>
#include <cmath>

#include "accessibility/a11y_log.h"

namespace a11y::race {
namespace {

// Curve grading, in radians of heading change per station - how much the track turns between one
// checkpoint and the next.
//
// This replaced a measure normalised by track width, which was badly wrong: a KMP checkpoint quad
// is far wider than the road it spans, so curvature times width overstated every corner and almost
// the whole lap graded as curving. Station spacing is a real length taken from the lap itself.
//
// `kTightnessNormal` is the game's own threshold for "this is a corner" - 0.3 rad of heading change
// between route segments - and the rest are doublings either side of it.
// kTightnessEnter lives in course_map.h, where the map itself reads straights against it.
constexpr float kTightnessNormal = 0.3f;    // the game's own corner threshold, 17.19 degrees
constexpr float kTightnessHard = 0.6f;
constexpr float kTightnessHairpin = 1.0f;

// A curve tolerates this many straight stations before it counts as having ended, so a single noisy
// checkpoint does not split one corner into two calls. A sign flip splits with no tolerance at all,
// which is what makes a chicane read as two curves instead of one blurred one.
constexpr int kCurveGapStations = 2;

// A run has to turn at least this much in total to be worth calling. Without it every wiggle in the
// checkpoint line becomes a corner, and the calls stop matching what the player feels. Expressed in
// degrees of total heading change, which is spacing-invariant - a minimum station count would not
// be.
constexpr float kCurveMinDegrees = 15.0f;

// A Hard curve that turns at least this much overall is really a hairpin, even if no single station
// is tight enough on its own.
constexpr float kHairpinDegrees = 120.0f;

// A curve long enough to be worth saying so, in stations.
constexpr float kLongCurveStations = 4.0f;

constexpr float kPi = 3.14159265358979f;
constexpr float kRadToDeg = 180.0f / kPi;

TurnSeverity SeverityFor(float peakTightness, float totalDegrees) {
    if (peakTightness >= kTightnessHairpin ||
        (peakTightness >= kTightnessHard && totalDegrees >= kHairpinDegrees)) {
        return TurnSeverity::Hairpin;
    }
    if (peakTightness >= kTightnessHard) {
        return TurnSeverity::Hard;
    }
    if (peakTightness >= kTightnessNormal) {
        return TurnSeverity::Normal;
    }
    return TurnSeverity::Easy;
}

// Shortest way round, so summing these accumulates past 180 degrees instead of clipping - a 230
// degree hairpin has to stay a 230 degree hairpin.
float WrapAngle(float radians) {
    while (radians > kPi) {
        radians -= 2.0f * kPi;
    }
    while (radians < -kPi) {
        radians += 2.0f * kPi;
    }
    return radians;
}

}  // namespace

float CourseMap::TightnessAt(int i) const {
    if (mTurn.empty()) {
        return 0.0f;
    }
    // The station's OWN span, not the lap mean: the threshold this is graded against is the game's
    // heading change between two authored route segments, so a course whose route points bunch up
    // in the corners must not have those corners scaled down by the average of the straights.
    return std::fabs(mTurn[Wrap(i)]) * SpanAt(i);
}

// Heading of the segment leaving this station.
float CourseMap::SegmentHeading(int i) const {
    float ax, az, bx, bz;
    Centre(i, ax, az);
    Centre(i + 1, bx, bz);
    return std::atan2(bx - ax, bz - az);
}

void CourseMap::EmitCurve(int entry, int end) {
    int steps = 0;
    for (int s = entry; s != end; s = Wrap(s + 1)) {
        if (++steps > StationCount()) {
            return;  // refuses to run away on a malformed map
        }
    }

    float peak = 0.0f;
    // The run starts because the track already turns AT `entry`, and that turn is part of the
    // corner: summing only from the next station cost a two-station corner half its arc, which
    // could grade it under the minimum and drop it, or let its tail decide its side. Centre wraps,
    // so the segment before station zero is the closing one rather than an out-of-range read.
    float totalTurn = WrapAngle(SegmentHeading(entry) - SegmentHeading(entry - 1));
    for (int k = 0; k <= steps; ++k) {
        const int idx = Wrap(entry + k);
        peak = std::max(peak, TightnessAt(idx));
        if (k < steps) {
            totalTurn += WrapAngle(SegmentHeading(idx + 1) - SegmentHeading(idx));
        }
    }

    const float totalDegrees = std::fabs(totalTurn) * kRadToDeg;
    if (totalDegrees < kCurveMinDegrees) {
        return;  // a wiggle in the checkpoint line, not a corner
    }

    Curve curve;
    curve.entry = entry;
    // The geometric middle, deliberately not the tightest station. A corner that loads up early has
    // its peak at the entry, and an apex beep there collides with the entry beep and leaves the
    // middle of the corner unmarked.
    curve.apex = Wrap(entry + steps / 2);
    curve.exit = end;
    // Direction from the summed turn, which is the same quantity the severity is graded from.
    // Raw heading deltas carry the world's chirality, not the settled convention: a positive sum
    // is a right turn only when "right" is (fz, -fx). Multiplying by the perpendicular sign the
    // checkpoint fixed maps it onto the one convention every other signed value uses - without
    // this the spoken side and the beep pan are mirrored on half the courses.
    curve.right = totalTurn * mRightPerpSign > 0.0f;
    curve.severity = SeverityFor(peak, totalDegrees);
    // The same peak the severity is graded from, kept continuous and against the same anchor.
    curve.intensity = peak / kTightnessNormal;
    curve.isLong = mMeanSpacing > 0.0f &&
                   ArcForward(entry, end) > kLongCurveStations * mMeanSpacing;
    mCurves.push_back(curve);
}

void CourseMap::BuildCurves() {
    const int n = StationCount();
    mCurves.clear();

    // Scanning starts at a straight station, so a corner that straddles the finish line is walked
    // in one piece instead of being cut in two. A lap with no straight at all is a single
    // continuous curve, which is exactly what an oval like Baby Park is, so it is emitted as one.
    int origin = -1;
    for (int i = 0; i < n; ++i) {
        if (TightnessAt(i) < kTightnessEnter) {
            origin = i;
            break;
        }
    }
    if (origin < 0) {
        EmitCurve(0, n - 1);
        if (!mCurves.empty()) {
            mCurves.back().isLong = true;
        }
        LogCurveMap();
        return;
    }

    int runStart = -1;
    int runSign = 0;
    int straightRun = 0;

    // One step past the lap, so the scan returns to the straight origin it began on.
    for (int step = origin; step <= origin + n; ++step) {
        const int i = Wrap(step);
        const float tightness = TightnessAt(i);
        const int sign = tightness < kTightnessEnter ? 0 : (mTurn[i] > 0.0f ? 1 : -1);

        const bool breaksRun =
            runStart >= 0 && ((sign != 0 && sign != runSign) || straightRun > kCurveGapStations);

        if (breaksRun) {
            EmitCurve(runStart, Wrap(step - straightRun - 1));
            runStart = -1;
            runSign = 0;
            straightRun = 0;
        }

        if (sign == 0) {
            if (runStart >= 0) {
                ++straightRun;
            }
            continue;
        }
        if (runStart < 0) {
            runStart = i;
            runSign = sign;
        }
        straightRun = 0;
    }

    // Returning to the origin cannot break a run - the origin is straight, so the sign test never
    // fires, and a run reaching it has at most `kCurveGapStations` straights behind it. Without
    // this the corner that wraps past the finish line is silently dropped, which happens on every
    // lap of every course whose station zero falls inside a corner.
    if (runStart >= 0) {
        EmitCurve(runStart, Wrap(origin - straightRun));
    }

    LogCurveMap();
}

// Temporary. Dumps what the segmentation actually produced, so a cue that lands in the wrong place
// can be traced to the map rather than to the cue. Remove once the curve cues are confirmed.
void CourseMap::LogCurveMap() const {
    RT_LOGF(RT_TAG_A11Y, "curve map: %d stations, lap %.0f, spacing %.0f, %d curves\n",
            StationCount(), static_cast<double>(mLapLength), static_cast<double>(mMeanSpacing),
            static_cast<int>(mCurves.size()));
    for (int i = 0; i < StationCount(); ++i) {
        RT_LOGF(RT_TAG_A11Y, "  station %2d turn=%+.3f rad corridor=%.0f\n", i,
                static_cast<double>(mTurn[i] * SpanAt(i)), static_cast<double>(HalfWidth(i)));
    }
    for (const Curve& c : mCurves) {
        RT_LOGF(RT_TAG_A11Y,
                "  curve entry=%d apex=%d exit=%d %s severity=%d int=%.2f long=%d arc=%.0f\n",
                c.entry, c.apex, c.exit, c.right ? "right" : "left",
                static_cast<int>(c.severity), static_cast<double>(c.intensity), c.isLong ? 1 : 0,
                static_cast<double>(ArcForward(c.entry, c.exit)));
    }
}

float CourseMap::CurveLength(const Curve& curve) const {
    return ArcForward(curve.entry, curve.exit);
}

float CourseMap::CurveProgress(const Curve& curve, int station) const {
    return ArcForward(curve.entry, station);
}

const Curve* CourseMap::ActiveCurve(int station, float clearance) const {
    return ActiveCurveAt(ArcAt(station), clearance);
}

// Everything here is measured as UNSIGNED forward distance around the lap. A signed measure folds
// at half a lap, so a corner longer than that - Baby Park's single continuous curve, and any oval
// emitted as one - reads as "behind" from inside its own first half and is dropped at its own
// entry.
const Curve* CourseMap::ActiveCurveAt(float fromArc, float clearance) const {
    const float lap = mLapLength;
    auto forward = [lap](float from, float to) {
        if (!(lap > 0.0f)) {
            return 0.0f;
        }
        const float d = std::fmod(to - from, lap);
        return d < 0.0f ? d + lap : d;
    };

    const Curve* entered = nullptr;
    float bestSince = 0.0f;
    const Curve* ahead = nullptr;
    float bestAhead = 0.0f;
    for (const Curve& curve : mCurves) {
        const float entryArc = ArcAt(curve.entry);
        // How far past this corner's entry the kart is. A corner stays entered until its exit is
        // `clearance` behind, so the choice does not flicker the moment the exit is crossed.
        const float since = forward(entryArc, fromArc);
        if (since <= CurveLength(curve) + clearance) {
            // The most recently entered wins: inside the second half of a chicane the first half
            // is still selectable, and describing it there is describing the wrong corner.
            if (entered == nullptr || since < bestSince) {
                entered = &curve;
                bestSince = since;
            }
            continue;
        }
        const float toEntry = forward(fromArc, entryArc);
        if (ahead == nullptr || toEntry < bestAhead) {
            ahead = &curve;
            bestAhead = toEntry;
        }
    }
    return entered != nullptr ? entered : ahead;
}

bool CourseMap::IsChainFollower(const Curve& curve, float gap) const {
    const Curve* previous = nullptr;
    float bestGap = 0.0f;
    for (const Curve& other : mCurves) {
        if (other.entry == curve.entry) {
            continue;
        }
        const float between = ArcForward(other.exit, curve.entry);
        if (previous == nullptr || between < bestGap) {
            previous = &other;
            bestGap = between;
        }
    }
    return previous != nullptr && bestGap <= gap;
}

const Curve* CourseMap::CurveAt(int station) const {
    for (const Curve& curve : mCurves) {
        if (CurveProgress(curve, station) <= CurveLength(curve)) {
            return &curve;
        }
    }
    return nullptr;
}

const Curve* CourseMap::NextCurve(int station) const {
    const Curve* best = nullptr;
    float bestAhead = 0.0f;
    for (const Curve& curve : mCurves) {
        const float ahead = ArcForward(station, curve.entry);
        if (best == nullptr || ahead < bestAhead) {
            best = &curve;
            bestAhead = ahead;
        }
    }
    return best;
}

}  // namespace a11y::race
