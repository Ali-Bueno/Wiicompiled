#include "accessibility/race/race_record.h"

#include <algorithm>

#include "accessibility/race/guest_read.h"
#include "accessibility/race/race_state.h"

namespace a11y::race {
namespace {

// Every address and offset was recovered from the translated body of the function named beside it.
// None of these functions is hookable: Raceinfo::Update, RaceinfoPlayer::Update and
// RaceinfoPlayer::UpdateCheckPoint are all short-circuited by MKW_TRANSLATED_TRAIT, and the small
// accessors are inlined as leaves with no call sites at all. Polling is the only option, and also
// the one that needs no retranslation.

constexpr std::uint32_t kRaceinfoPtr = 0x809BD730;  // Raceinfo::CreateInstance (0x80532084)
constexpr std::uint32_t kRacedataPtr = 0x809BD728;  // Racedata::CreateInstance (0x8052FE58)

constexpr std::uint32_t kRaceinfoPlayers = 0x0C;  // RaceinfoPlayer**, stride 4
constexpr std::uint32_t kRaceinfoTimer = 0x20;    // u32, counts up in frames
constexpr std::uint32_t kRaceinfoStage = 0x28;    // s32, what IsAtLeastStage compares

// Raceinfo::Update assigns these, and IsAtLeastStage(2) is the game's own "is it live" test.
constexpr std::int32_t kStageIntro = 0;
constexpr std::int32_t kStageCountdown = 1;
constexpr std::int32_t kStageRacing = 2;
// Update latches 3 when the race can end and promotes it to 4 on the next frame, so anything at or
// past 3 is over.
constexpr std::int32_t kStageFinished = 3;

// Raceinfo::GetRemainingCountdownTime returns this minus the timer, so the countdown is over when
// the timer reaches it - which is exactly when Update flips the stage to racing.
constexpr std::int32_t kCountdownFrames = 240;

constexpr std::uint32_t kPlayerCheckpointWord = 0x08;  // u16 index at +0x0A, the word's low half
constexpr std::uint32_t kPlayerCompletion = 0x0C;      // float, lap plus fraction of the lap
constexpr std::uint32_t kPlayerMaxCompletion = 0x10;   // float, the monotonic high-water mark
constexpr std::uint32_t kPlayerPositionWord = 0x20;    // u8 position at +0x20, 1-based
constexpr std::uint32_t kPlayerLapWord = 0x24;         // s16 lap at +0x24, u8 total laps at +0x26
// RaceinfoPlayer::Update sets this bit when the kart's movement runs against the checkpoint's own
// forward normal. The game's own answer, so no threshold of ours can disagree with the game about
// which way round the course runs.
constexpr std::uint32_t kPlayerFlags = 0x38;
constexpr std::uint32_t kPlayerFlagWrongWay = 0x4;

constexpr std::uint32_t kRacedataPlayerCount = 0x024;    // u8
constexpr std::uint32_t kRacedataLocalToPlayer = 0xB84;  // s8[4], indexed by splitscreen slot
constexpr std::uint32_t kRacedataKmpLapCount = 0xB8D;    // u8
constexpr std::uint32_t kRacedataFlags = 0xB90;          // u32; bit 2 validates the lap count above


// Player one. Splitscreen would pass 1..3 here; the mod reads only the first local player, matching
// the menu narration, which reads only player one's focus.
constexpr std::uint32_t kLocalSlot = 0;

// Raceinfo::GetLapCount falls back to this whenever the course value is absent or out of range.
constexpr int kDefaultLapCount = 3;
constexpr int kMinLapCount = 1;
constexpr int kMaxLapCount = 9;
constexpr std::uint32_t kLapCountValidBit = 4;

// Reproduces Raceinfo::GetLapCount (0x805336A4), which reads Racedata rather than KMP STGI - so it
// is the total the HUD shows, including the VS and menu overrides that STGI would miss.
//
// Returns whether the total is really the race's own. The game's fallback of 3 is still written out
// because that is what its HUD would show, but a caller that would END the player's race on it has
// to know it is a guess: battle and other lap-less modes reach here too.
bool EffectiveLapCount(std::uint32_t racedata, int& lapsOut) {
    lapsOut = kDefaultLapCount;
    std::uint32_t flags = 0;
    std::uint8_t laps = 0;
    if (!Memory::TryRead32(racedata + kRacedataFlags, flags) ||
        (flags & kLapCountValidBit) == 0 ||
        !TryU8(racedata + kRacedataKmpLapCount, laps)) {
        return false;
    }
    const int value = static_cast<int>(laps);
    if (value < kMinLapCount || value > kMaxLapCount) {
        return false;
    }
    lapsOut = value;
    return true;
}

// Last tick's value of the race frame counter, and whether there is one to compare against.
// File-static because the pause test is a diff and RaceState is rebuilt every frame.
std::uint32_t g_lastRaceFrame = 0;
bool g_haveLastRaceFrame = false;
int g_stalledTicks = 0;

// Two consecutive stalled ticks, not one: the same guest frame presented twice would otherwise read
// as a pause and drop the assists for a frame.
constexpr int kPauseStallTicks = 2;

// The pause menu keeps presenting frames, so our tick keeps running while the game's own race frame
// counter (Raceinfo+0x20, which counts on past the countdown) stands still. That stall IS the pause;
// no new hook is needed. Zero is excluded so a race that has not started counting yet does not read
// as paused.
bool RaceFrameStalled(std::uint32_t raceinfo) {
    std::uint32_t frame = 0;
    if (!Memory::TryRead32(raceinfo + kRaceinfoTimer, frame)) {
        ResetRaceRecord();
        return false;
    }
    if (g_haveLastRaceFrame && frame == g_lastRaceFrame && frame != 0) {
        ++g_stalledTicks;
    } else {
        g_stalledTicks = 0;
    }
    g_lastRaceFrame = frame;
    g_haveLastRaceFrame = true;
    return g_stalledTicks >= kPauseStallTicks;
}

// Racedata::GetPlayerIdOfLocalPlayer (0x80531F70): a sign-extended byte per splitscreen slot.
bool LocalPlayerRecord(std::uint32_t racedata, std::uint32_t raceinfo, std::uint32_t& recordOut,
                       int& idOut) {
    std::uint8_t rawId = 0;
    std::uint8_t players = 0;
    if (!TryU8(racedata + kRacedataLocalToPlayer + kLocalSlot, rawId) ||
        !TryU8(racedata + kRacedataPlayerCount, players)) {
        return false;
    }
    const int id = static_cast<std::int8_t>(rawId);
    if (id < 0 || id >= static_cast<int>(players)) {
        return false;
    }

    std::uint32_t array = 0;
    if (!TryPointer(raceinfo + kRaceinfoPlayers, array) ||
        !TryPointer(array + static_cast<std::uint32_t>(id) * kPointerStride, recordOut)) {
        return false;
    }
    idOut = id;
    return true;
}

}  // namespace

void ResetRaceRecord() {
    g_lastRaceFrame = 0;
    g_haveLastRaceFrame = false;
    g_stalledTicks = 0;
}

void FillRaceRecord(RaceState& state) {
    std::uint32_t raceinfo = 0;
    std::uint32_t racedata = 0;
    if (!TryPointer(kRaceinfoPtr, raceinfo) || !TryPointer(kRacedataPtr, racedata)) {
        ResetRaceRecord();
        return;
    }

    state.paused = RaceFrameStalled(raceinfo);

    std::uint32_t stageWord = 0;
    if (!Memory::TryRead32(raceinfo + kRaceinfoStage, stageWord)) {
        return;
    }
    const std::int32_t stage = static_cast<std::int32_t>(stageWord);

    // Only stage 2 is driving. Intro and countdown come before it, and the two finished stages
    // after it - so the assists go quiet on their own at the flag, with no extra bookkeeping. A
    // paused race is not driving either: the kart is frozen behind the menu.
    state.driving = stage == kStageRacing && !state.paused;
    state.finished = stage >= kStageFinished;

    if (stage == kStageIntro || stage == kStageCountdown) {
        std::uint32_t timer = 0;
        if (Memory::TryRead32(raceinfo + kRaceinfoTimer, timer)) {
            state.countdownFrames =
                std::max(0, kCountdownFrames - static_cast<std::int32_t>(timer));
        }
    }

    const bool lapsKnown = EffectiveLapCount(racedata, state.totalLaps);

    std::uint32_t record = 0;
    if (!LocalPlayerRecord(racedata, raceinfo, record, state.playerId)) {
        return;
    }

    std::uint16_t checkpoint = 0;
    if (TryU16(record + kPlayerCheckpointWord, /*highHalf=*/false, checkpoint)) {
        state.checkpoint = static_cast<int>(checkpoint);
    }

    std::uint32_t positionWord = 0;
    if (Memory::TryRead32(record + kPlayerPositionWord, positionWord)) {
        state.position = static_cast<int>(positionWord >> 24);
    }

    std::uint16_t lap = 0;
    if (TryU16(record + kPlayerLapWord, /*highHalf=*/true, lap)) {
        state.lap = static_cast<std::int16_t>(lap);
        // The stage is race-wide, so it stays on "racing" while the other karts finish. This player
        // is done once their lap passes the total, which is the same test EndLap uses - without it
        // the guide keeps sounding through the post-finish auto-drive. Only when the total is the
        // race's own: against the guessed 3 this would end every lap-less mode on lap 4.
        if (lapsKnown && state.totalLaps > 0 && state.lap > state.totalLaps) {
            state.driving = false;
            state.finished = true;
        }
    }

    std::uint32_t flags = 0;
    if (Memory::TryRead32(record + kPlayerFlags, flags)) {
        state.wrongWay = (flags & kPlayerFlagWrongWay) != 0;
    }

    // The monotonic high-water mark, not the live completion: it never steps backwards, and a
    // progress value that jitters is exactly what makes phase transitions fire twice.
    if (!TryFloat(record + kPlayerMaxCompletion, state.completion)) {
        TryFloat(record + kPlayerCompletion, state.completion);
    }
}

}  // namespace a11y::race
