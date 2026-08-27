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
constexpr float kTightnessEnter = 0.15f;    // below this the track reads as straight
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
    // Curvature is radians per unit of travel, so multiplying by the spacing gives the heading
    // change from one station to the next.
    return std::fabs(mTurn[Wrap(i)]) * mMeanSpacing;
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
    float totalTurn = 0.0f;
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
                static_cast<double>(mTurn[i] * mMeanSpacing),
                static_cast<double>(HalfWidth(i)));
    }
    for (const Curve& c : mCurves) {
        RT_LOGF(RT_TAG_A11Y, "  curve entry=%d apex=%d exit=%d %s severity=%d long=%d arc=%.0f\n",
                c.entry, c.apex, c.exit, c.right ? "right" : "left",
                static_cast<int>(c.severity), c.isLong ? 1 : 0,
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
    const Curve* best = nullptr;
    float bestEntry = 0.0f;
    for (const Curve& curve : mCurves) {
        // A curve stays selectable until its exit is well behind, so the choice does not flicker to
        // the next corner the moment this one's exit is crossed.
        if (ArcSigned(station, curve.exit) <= -clearance) {
            continue;
        }
        const float toEntry = ArcSigned(station, curve.entry);
        if (best == nullptr || toEntry < bestEntry) {
            best = &curve;
            bestEntry = toEntry;
        }
    }
    return best;
}

const Curve* CourseMap::ActiveCurveAt(float fromArc, float clearance) const {
    const Curve* best = nullptr;
    float bestEntry = 0.0f;
    for (const Curve& curve : mCurves) {
        if (ArcSignedTo(fromArc, curve.exit) <= -clearance) {
            continue;
        }
        const float toEntry = ArcSignedTo(fromArc, curve.entry);
        if (best == nullptr || toEntry < bestEntry) {
            best = &curve;
            bestEntry = toEntry;
        }
    }
    return best;
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
