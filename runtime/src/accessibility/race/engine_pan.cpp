#include "accessibility/race/engine_pan.h"

#include <algorithm>

#include "accessibility/a11y_log.h"
#include "accessibility/race/guest_read.h"
#include "accessibility/race/race_state.h"
#include "runtime_config.h"

namespace a11y::race {
namespace {

// Every address and offset recovered from the translated bodies, never guessed.

// Audio::RaceMgr singleton, from the translated body that loads it as 0x809C0000 + 10232.
constexpr std::uint32_t kAudioRaceMgrPtr = 0x809C27F8;
constexpr std::uint32_t kRaceMgrKartActors = 0x18;  // Audio::KartActor*[4]
constexpr std::uint32_t kRaceMgrActorCount = 0x28;  // u8
constexpr std::uint32_t kMaxKartActors = 4;

constexpr std::uint32_t kActorEngineSound = 0xBC;  // BasicSound*, null-checked by the game itself
constexpr std::uint32_t kActorKartObject = 0xDC;   // the kart this actor follows
constexpr std::uint32_t kKartObjectIdWord = 156;   // u16 player id, in the word's high half

// nw4r::snd::detail::BasicSound. UpdateParam adds these three together, unclamped, to get the pan
// the mixer actually uses, so writing the external term can cancel the other two exactly.
constexpr std::uint32_t kSoundAmbientPan = 56;  // refilled every frame by the 3D audio engine
constexpr std::uint32_t kSoundActorPan = 84;    // the sound actor's own pan
constexpr std::uint32_t kSoundExternalPan = 0xA8;  // what BasicSound::SetPan writes; nothing else
                                                  // touches it for a kart engine

// The sign of this field was re-derived twice by inference and BOTH derivations were contradicted
// by the player's ear on the next run - the lesson is that nothing upstream of the speaker can
// settle it. What IS settled, by the player's own toggle tests, is the END-TO-END product for the
// steering law in force: under pure pursuit (2026-08-27, first completed race) it is
// kRightIsPositive x invert_steering_pan(default false) that produces their validated "curva a la
// izquierda -> motor a la derecha"; under the earlier two-term law the same ear pinned invert
// true. A change of steering law re-decides the SETTING, never this constant.
constexpr float kRightIsPositive = 1.0f;

// The mixer's sum is external + ambient + actor, each nominally within [-1, 1]. The external term
// has to cancel the other two AND still place the result anywhere in [-1, 1], so its own bound is
// 3. Clamping it at 1, as an earlier draft did, could flip the delivered side outright when the
// game's own placement was extreme - a sign inversion in the one layer that already produced one.
constexpr float kPanLimit = 3.0f;

// Why the last attempt stopped. Logged once per change so a failing chain says which link broke
// instead of just going quiet.
enum class Stage {
    None,
    NoRaceState,
    NoAudioManager,
    NoActorMatched,
    NoEngineSound,
    NoPanFields,
    WriteFailed,
    Applied,
};

const char* StageName(Stage stage) {
    switch (stage) {
        case Stage::NoRaceState:
            return "race state not readable";
        case Stage::NoAudioManager:
            return "Audio::RaceMgr not readable";
        case Stage::NoActorMatched:
            return "no kart actor matched the player";
        case Stage::NoEngineSound:
            return "engine sound not playing";
        case Stage::NoPanFields:
            return "pan fields not readable";
        case Stage::WriteFailed:
            return "pan write rejected";
        case Stage::Applied:
            return "applied";
        default:
            return "none";
    }
}

// Finds the audio actor following the player's kart.
//
// Matched by pointer first. The mod already knows the player's kart object, and comparing addresses
// needs nothing trusted; the player-id field is only a fallback for the case where the audio actor
// links to a different object than the kart manager hands out.
Stage LocalKartActor(const RaceState& state, std::uint32_t& actorOut) {
    std::uint32_t manager = 0;
    std::uint8_t count = 0;
    if (!TryPointer(kAudioRaceMgrPtr, manager) || !TryU8(manager + kRaceMgrActorCount, count)) {
        return Stage::NoAudioManager;
    }

    const std::uint32_t total = std::min<std::uint32_t>(count, kMaxKartActors);
    std::uint32_t byId = 0;

    for (std::uint32_t i = 0; i < total; ++i) {
        std::uint32_t actor = 0;
        std::uint32_t kart = 0;
        if (!TryPointer(manager + kRaceMgrKartActors + i * kPointerStride, actor) ||
            !TryPointer(actor + kActorKartObject, kart)) {
            continue;
        }
        if (state.kartObject != 0 && kart == state.kartObject) {
            actorOut = actor;
            return Stage::Applied;
        }
        std::uint16_t id = 0;
        if (byId == 0 && state.playerId >= 0 &&
            TryU16(kart + kKartObjectIdWord, /*highHalf=*/true, id) &&
            static_cast<int>(id) == state.playerId) {
            byId = actor;
        }
    }

    if (byId != 0) {
        actorOut = byId;
        return Stage::Applied;
    }
    return Stage::NoActorMatched;
}

}  // namespace

void EnginePan::Reset() {
    mAnnouncedSlot = false;
    mIdleRestored = false;
    mLastStage = static_cast<int>(Stage::None);
}

void EnginePan::Apply(const RaceState& state, float pan, bool active) {
    if (!active) {
        // Not guiding. Writing our cancellation term while idle would force the engine to the
        // centre; zeroing the external term once gives the game its own placement back. One
        // attempt per transition - a sound that is not playing has nothing to restore.
        if (!mIdleRestored) {
            mIdleRestored = true;
            std::uint32_t actor = 0;
            std::uint32_t sound = 0;
            if (state.valid && LocalKartActor(state, actor) == Stage::Applied &&
                TryPointer(actor + kActorEngineSound, sound)) {
                TryWriteFloat(sound + kSoundExternalPan, 0.0f);
            }
        }
        return;
    }
    mIdleRestored = false;

    Stage stage = Stage::Applied;
    std::uint32_t sound = 0;

    if (!state.valid) {
        stage = Stage::NoRaceState;
    } else {
        std::uint32_t actor = 0;
        stage = LocalKartActor(state, actor);
        if (stage == Stage::Applied) {
            // Never cached: engine sounds come from a pool and the handle is nulled when one is
            // stopped or stolen, which is why the game null-checks it before every use.
            if (!TryPointer(actor + kActorEngineSound, sound)) {
                stage = Stage::NoEngineSound;
            }
        }
    }

    if (stage == Stage::Applied) {
        float ambient = 0.0f;
        float actorPan = 0.0f;
        if (!TryFloat(sound + kSoundAmbientPan, ambient) ||
            !TryFloat(sound + kSoundActorPan, actorPan)) {
            stage = Stage::NoPanFields;
        } else {
            // The mixer uses external + ambient + actor. Subtracting the other two turns our term
            // into an absolute pan, so the camera-relative placement is replaced rather than merely
            // nudged - while the engine's own speed-linked volume and pitch carry on untouched.
            //
            // Whether the engine leans towards the line or away from it is a preference, not a
            // correctness question - some players steer towards the sound, some away from it.
            const float preference =
                RuntimeConfigFile::AccessibilityInvertSteeringPan() ? -1.0f : 1.0f;
            const float desired =
                std::clamp(pan, -1.0f, 1.0f) * kRightIsPositive * preference;
            const float external =
                std::clamp(desired - (ambient + actorPan), -kPanLimit, kPanLimit);
            if (!TryWriteFloat(sound + kSoundExternalPan, external)) {
                stage = Stage::WriteFailed;
            }
        }
    }

    // Temporary. Says which link of the chain broke, so a silent engine is diagnosable from the log
    // rather than by guesswork. Remove once the pan is confirmed working on a real run.
    if (static_cast<int>(stage) != mLastStage) {
        mLastStage = static_cast<int>(stage);
        RT_LOGF(RT_TAG_A11Y, "engine pan: %s (player %d, kart %08x, sound %08x)\n",
                StageName(stage), state.playerId, state.kartObject, sound);
    }
}

}  // namespace a11y::race
