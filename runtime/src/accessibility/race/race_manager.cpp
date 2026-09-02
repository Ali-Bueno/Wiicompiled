#include "accessibility/race/race_manager.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

#include "accessibility/a11y_log.h"
#include "accessibility/audio/cue_service.h"
#include "accessibility/race/course_map.h"
#include "accessibility/race/drive_assist.h"
#include "accessibility/race/edge_map.h"
#include "accessibility/race/engine_pan.h"
#include "accessibility/race/heading.h"
#include "accessibility/race/item_beacon.h"
#include "accessibility/race/guest_read.h"
#include "accessibility/race/kart_volume.h"
#include "accessibility/race/kmp_reader.h"
#include "accessibility/race/race_narrator.h"
#include "accessibility/race/race_state.h"
#include "accessibility/race/roulette_volume.h"
#include "accessibility/race/track_limits.h"
#include "runtime_config.h"

namespace a11y::race {
namespace {

CourseMap g_map;
DriveAssist g_driveAssist;
TrackLimits g_trackLimits;
RaceNarrator g_narrator;
ItemBeacon g_itemBeacon;
EnginePan g_enginePan;
KartVolume g_kartVolume;
RouletteVolume g_rouletteVolume;

// Shared so every signed cue in the mod - steering error, curve side, beacon bearing - agrees on
// which way "right" is. Forgotten on course change: the sign re-derives from the next map's vote.
Handedness g_handedness;

std::uint64_t g_courseSignature = 0;
int g_lastStation = -1;

// Last frame's readable/finished state, so a race restarting on the SAME course - which keeps the
// course signature and so never reaches ForgetCourse - still gets its per-race services cleared.
bool g_prevValid = false;
bool g_prevFinished = false;

// The lap the road shift last saw. A lap boundary is one of the two moments where moving the line
// under the kart is harmless; -1 means there has been no lap to compare against yet.
int g_lastShiftLap = -1;

// Frames spent retrying a course whose routes read but did not close a sane lap. Early frames can
// catch the KMP mid-life; past the window the checkpoint-ordered fallback is accepted (progress
// and corner cues still work; the steering guide gates itself off RouteBased).
int g_routeRetryFrames = 0;

std::chrono::steady_clock::time_point g_lastTick;
bool g_timing = false;

// A paused game or a long load would otherwise arrive as one enormous time step and fire every
// timed cue at once.
constexpr float kMaxDtSec = 0.25f;

// Real elapsed time, which is what the cue timers run on - a beep interval is a wall-clock
// interval. It is deliberately NOT what the kart's speed is converted with; see below.
float StepSeconds() {
    const auto now = std::chrono::steady_clock::now();
    if (!g_timing) {
        g_timing = true;
        g_lastTick = now;
        return 0.0f;
    }
    const float dt = std::min(std::chrono::duration<float>(now - g_lastTick).count(), kMaxDtSec);
    g_lastTick = now;
    return dt;
}

// How many game frames go by in a second. The kart's speed is units per GUEST frame and the guest
// advances one frame per video retrace, so this - not the host's frame time - is what turns it
// into units per second. Dividing by a measured frame duration tied every "seconds of lead"
// threshold in the mod to the host: a stutter made the corner calls late and a fast presenter made
// them early, while the kart drove exactly the same.
//
// Read from the VI HLE's own TV format global, which it writes into guest memory at every retrace
// (runtime/src/hle/vi.cpp:139,227), and mapped by the interval that HLE picks for it
// (IntervalForFormat, runtime/src/hle/vi.cpp:155-158): PAL 20000 us, everything else 16666 us.
constexpr std::uint32_t kGuestViTvFormatAddr = 0x80386BA8;
constexpr std::uint32_t kGuestViTvFormatPal = 1;
constexpr float kGuestPalFramesPerSecond = 50.0f;
constexpr float kGuestDefaultFramesPerSecond = 60.0f;

float GuestFramesPerSecond() {
    std::uint32_t format = 0;
    if (Memory::TryRead32(kGuestViTvFormatAddr, format) && format == kGuestViTvFormatPal) {
        return kGuestPalFramesPerSecond;
    }
    return kGuestDefaultFramesPerSecond;
}

// What one race owns, as opposed to what the course owns. Cleared whenever a race begins, so a
// retry cannot inherit the previous race's spoken lap, position or finish. Only the services: the
// frame's RaceState has already been read by the time this runs mid-tick.
void ForgetRace() {
    g_narrator.Reset();
    g_driveAssist.Reset();
    g_trackLimits.Reset();
    g_itemBeacon.Reset();
}

void ForgetCourse() {
    g_map.Clear();
    g_lastStation = -1;
    g_routeRetryFrames = 0;
    g_lastShiftLap = -1;
    g_prevValid = false;
    g_prevFinished = false;
    g_handedness.Forget();
    g_enginePan.Reset();
    g_kartVolume.Reset();
    g_rouletteVolume.Reset();
    EdgeMap::Reset();
    ForgetRace();
    // The surface latch lives here and is file-static, so without this the last course's off-road
    // is announced on the new course's grid.
    ResetRaceState();
}

// Temporary. One line whenever the readable/driving state changes, so a silent race says which of
// the two gates is shut instead of leaving it to guesswork. Remove once the race cues are confirmed.
void LogStateChange(const RaceState& state) {
    static int lastValid = -1;
    static int lastDriving = -1;
    const int valid = state.valid ? 1 : 0;
    const int driving = state.driving ? 1 : 0;
    if (valid == lastValid && driving == lastDriving) {
        return;
    }
    lastValid = valid;
    lastDriving = driving;
    RT_LOGF(RT_TAG_A11Y,
            "race state: valid=%d driving=%d finished=%d player=%d kart=%08x cp=%d lap=%d/%d "
            "pos=%d speed=%.2f ratio=%.2f\n",
            valid, driving, state.finished ? 1 : 0, state.playerId, state.kartObject,
            state.checkpoint, state.lap, state.totalLaps, state.position,
            static_cast<double>(state.speed), static_cast<double>(state.speedRatio));
}

// Temporary telemetry, about once a second while driving. Answers three questions at once: whether
// the course direction agrees with the kart's, where the kart sits across the track, and which way
// the guide is pointing. Remove once the steering cue is confirmed.
void LogTelemetry(const RaceState& state, const CourseMap& map, int station, float pan,
                  float bearingDeg, float horizonUnits) {
    static int countdown = 0;
    if (!state.valid || !state.driving || !map.Loaded()) {
        return;
    }
    if (--countdown > 0) {
        return;
    }
    countdown = 60;

    const float arc = map.ArcOfPosition(state.x, state.z, station);
    // Both frames at the kart's own ARC, not at its checkpoint-mapped station. With 25 checkpoints
    // for 48 stations the mapped station can be a station and a half away, which at 0.3-0.45 rad of
    // turn per station is up to 49 degrees of tangent error inside a corner - enough to print
    // align=1.00 for a kart that is nowhere near pointing along the line, which is exactly how a
    // whole diagnosis went wrong once.
    float trackX = 0.0f, trackZ = 0.0f;
    map.ForwardAtArc(arc, trackX, trackZ);
    float rightX = 0.0f, rightZ = 0.0f;
    map.RightVectorAtArc(arc, rightX, rightZ);
    const float alignment = state.forwardX * trackX + state.forwardZ * trackZ;
    // RoadOffsetAtArc returns the offset NORMALISED by the KMP corridor, and on a course whose
    // corridor is 51 that reads 20x smaller than the world units every other number here is in.
    // Both are printed, world units first, because a mixed-unit log is worse than no log.
    float lateral = 0.0f, corridor = 0.0f;
    const bool haveLateral =
        map.RoadOffsetAtArc(arc, state.x, state.y, state.z, lateral, nullptr, nullptr, &corridor);
    const float lateralUnits = lateral * corridor;
    // The real road on the side the kart is on, so a lateral can be read against the asphalt rather
    // than against the corridor the course happens to declare.
    float realDistance = 0.0f;
    EdgeKind edgeKind = EdgeKind::Unknown;
    if (!EdgeMap::SideAtArc(map, arc, lateralUnits > 0.0f, realDistance, edgeKind)) {
        realDistance = 0.0f;
    }

    // `bearing` is the guide's own input: degrees from the kart's heading to its aim point on the
    // line, positive to the kart's right, and `horizon` how far along the line that point is.
    RT_LOGF(RT_TAG_A11Y,
            "telemetry: cp=%d arc=%.0f kartfwd=(%.2f,%.2f) trackfwd=(%.2f,%.2f) "
            "trackright=(%.2f,%.2f) align=%.2f lat_u=%.0f road=%.0f lateral=%.2f(%d) "
            "bearing=%+.1f horizon=%.0f pan=%.2f speed=%.1f\n",
            state.checkpoint, static_cast<double>(arc), static_cast<double>(state.forwardX),
            static_cast<double>(state.forwardZ), static_cast<double>(trackX),
            static_cast<double>(trackZ), static_cast<double>(rightX),
            static_cast<double>(rightZ), static_cast<double>(alignment),
            static_cast<double>(lateralUnits), static_cast<double>(realDistance),
            static_cast<double>(lateral), haveLateral ? 1 : 0,
            static_cast<double>(bearingDeg), static_cast<double>(horizonUnits),
            static_cast<double>(pan), static_cast<double>(state.speed));
}

// Where on the course the player is. The game's own checkpoint index is preferred over searching
// the map: it is what the game itself races by, so it is right through crossovers and branches
// where a nearest-point search would jump to the wrong side of the track.
int ResolveStation(const RaceState& state) {
    // The game's checkpoint index indexes the checkpoint list, not the route, so it is translated
    // through the mapping built with the course.
    const int mapped = g_map.StationForCheckpoint(state.checkpoint);
    if (mapped >= 0) {
        return mapped;
    }
    return g_map.NearestStation(state.x, state.z, g_lastStation);
}

}  // namespace

void Reset() {
    g_courseSignature = 0;
    g_timing = false;
    ForgetCourse();
}

void InvalidateCourseMap() {
    // The signature stays: only the map is dropped, so the next Tick rebuilds it in place.
    ForgetCourse();
}

void Tick() {
    // Checked every frame rather than only while unloaded: the signature identifies the parsed
    // course, so a new race that reuses the manager's address is still caught.
    const std::uint64_t signature = CourseSignature();
    if (signature != g_courseSignature) {
        g_courseSignature = signature;
        ForgetCourse();
    }

    // Retried until it succeeds rather than attempted once: the manager exists a little before it
    // has finished parsing, so the first frames after a course appears will not read yet.
    if (g_courseSignature != 0 && !g_map.Loaded()) {
        // The route carries the geometry; the checkpoints only anchor the game's progress index
        // and settle which side is "right". The item route is preferred (it measured more central
        // than the CPUs' lane); if it does not read or does not close a lap, the CPU route is the
        // fallback, whose walk starts at the game's own cached start point.
        std::vector<RoutePoint> route;
        std::vector<Checkpoint> checkpoints;
        if (ReadCourseCheckpoints(checkpoints)) {
            bool built = false;
            if (RuntimeConfigFile::AccessibilityLineFromItemRoute() &&
                ReadCourseItemRoute(route)) {
                built = g_map.Build(std::move(route), 0, checkpoints);
                if (!built) {
                    RT_LOGF(RT_TAG_A11Y,
                            "course map: item route unusable, falling back to CPU route\n");
                }
            }
            if (!built && ReadCourseRoute(route)) {
                // A failed KTPT read costs the walk its start point, never the whole route.
                std::uint8_t startPoint = 0;
                ReadRouteStartPoint(startPoint);
                built = g_map.Build(std::move(route), startPoint, checkpoints);
            }
            if (built) {
                g_routeRetryFrames = 0;
            } else if (g_map.Loaded()) {
                // Neither source closed a sane lap. Retry for a while - early frames can catch
                // the KMP mid-life - then accept the checkpoint-ordered fallback and say so.
                if (++g_routeRetryFrames < kCourseSettleFrames) {
                    g_map.Clear();
                } else {
                    RT_LOGF(RT_TAG_A11Y,
                            "course map: WARNING no route closed a lap; checkpoint ordering "
                            "only, steering guide disabled\n");
                }
            }
        }
    }

    const float stepSec = StepSeconds();
    RaceState& state = ReadRaceState();
    state.speedPerSecond = state.speed * GuestFramesPerSecond();
    state.frameSec = 1.0f / GuestFramesPerSecond();

    // The real road edges, measured a couple of stations per tick until the course is covered,
    // then the racing line placed inside them around the kart's own size.
    EdgeMap::Tick(g_map, state.valid ? state.bodyHalfWidth : 0.0f, state.frameSec);
    // Behind the pause menu the game advances no time, so neither may any cue timer.
    const float dtSec = state.paused ? 0.0f : stepSec;
    // The cue renderer works in whole guest frames too, and takes their duration from here rather
    // than measuring the host's.
    if (state.frameSec > 0.0f) {
        audio::CueService::Instance().SetFramePeriod(state.frameSec);
    }

    LogStateChange(state);

    // A race restarted on the same course keeps the course signature, so ForgetCourse never runs
    // and the finish would already be spoken - and the lap and position suppressed - on lap one.
    if ((state.valid && !g_prevValid) || (state.driving && g_prevFinished)) {
        ForgetRace();
    }
    g_prevValid = state.valid;
    g_prevFinished = state.finished;

    // Once the edges are known the line becomes the racing line inside them (EdgeMap). Deferred to a moment where moving the line under the kart cannot teleport the guide: the kart
    // not driving, or a lap boundary, whichever comes first.
    // Only a READ lap counts: a failed read blanks `lap` to 0 and would fake a lap boundary.
    const bool lapChanged = state.valid && g_lastShiftLap >= 0 && state.lap != g_lastShiftLap;
    if (state.valid) {
        g_lastShiftLap = state.lap;
    }
    // `state.valid &&`: a failed guest read blanks the state, and `driving` goes false with it -
    // which is not the kart standing still, and is no moment to move the line under it.
    if (EdgeMap::Ready() && !g_map.RoadShifted() &&
        ((state.valid && !state.driving) || lapChanged)) {
        g_map.ApplyRoadShift(EdgeMap::Shifts());
    }

    const bool geometry = state.valid && g_map.Loaded();
    // The services are called even when there is nothing to read, because that is how they know to
    // stop whatever they were playing. Each re-checks the map before touching it.
    const int station = geometry ? ResolveStation(state) : 0;
    if (geometry) {
        g_lastStation = station;
        g_handedness.Observe(state, g_map, station);
    }

    g_driveAssist.Tick(state, g_map, g_handedness, station, dtSec);
    // The steering guide's only output, applied to the game's own engine note. When not guiding
    // the engine is handed back to the game rather than pinned to the centre.
    //
    // The pan is written as an ABSOLUTE placement, replacing the game's own, so it is only claimed
    // when there is a real line to steer along. Without the geometry the assist has nothing to aim
    // at and outputs a centred zero, and that zero would sit on the engine for the whole race,
    // destroying the camera-relative placement a sighted player hears.
    const bool guiding = geometry && g_map.RouteBased() && g_handedness.Known() && state.driving &&
                         RuntimeConfigFile::AccessibilitySteeringStrength() > 0;
    g_enginePan.Apply(state, g_driveAssist.SteeringPan(), guiding);
    // Volume, unlike the pan, applies to every kart - the rivals' knob is the whole point.
    g_kartVolume.Tick(state);
    g_rouletteVolume.Tick(state);
    LogTelemetry(state, g_map, station, g_driveAssist.SteeringPan(),
                 g_driveAssist.LastBearingDeg(), g_driveAssist.LastHorizonUnits());
    g_trackLimits.Tick(state, g_map, g_handedness, station, dtSec);
    g_itemBeacon.Tick(state, g_map, g_handedness, station, dtSec);

    // Speaks whether or not the course map read: lap and position come from the race record, not
    // from the geometry.
    g_narrator.Tick(state, dtSec);
}

}  // namespace a11y::race
