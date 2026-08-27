#include "accessibility/race/kart_volume.h"

#include <algorithm>

#include "accessibility/race/guest_read.h"
#include "accessibility/race/nw4r_sound.h"
#include "accessibility/race/race_state.h"
#include "runtime_config.h"

namespace a11y::race {
namespace {

// Kart::Player::CreateSound (0x8058ECE0) / Kart::PlayerBike::CreateSound (0x8058F56C) store the
// kart's own Audio::KartActor here. Every kart has one, CPU rivals included - the RaceMgr array
// at +0x18 holds only the up-to-4 local players and is the wrong place to look for rivals.
constexpr std::uint32_t kPlayerAudioActor = 0x38;

// The actor's BasicSound* slots, from the loop in Audio::KartActor::StopAllSound (0x80707F7C):
// engine +0xBC, drift/mini-turbo/surface +0xC0..+0xC8, and the four one-shot pool slots
// +0x80..+0x8C (boost, horn, collision). Pooled - a null slot means silent right now, and the
// pointers are re-read every frame because a stopped sound's slot is stolen.
constexpr std::uint32_t kActorSoundSlots[] = {0xBC, 0xC0, 0xC4, 0xC8, 0x80, 0x84, 0x88, 0x8C};

// The character VOICES live on a separate per-kart Audio::CharacterActor, not on the KartActor:
// Kart::Player::CreateModel (0x8058F820) stores the kart's DriverController at +0x18, and
// DriverController::__ct (0x807C7364) stores its CharacterActor at +0x0C (the chain
// Kart::Link::GetCharacterActor 0x805907A0 reads). Same path for the player and every rival.
constexpr std::uint32_t kPlayerDriverController = 0x18;
constexpr std::uint32_t kDriverCharacterActor = 0x0C;

// The CharacterActor's two nw4r::snd::SoundHandle slots - main voice +0xF8, queued voice +0x104;
// its destructor (0x80866C08) detaches exactly these two, and a SoundHandle is just the
// BasicSound* itself (SoundHandle::detail_AttachSound 0x800A2E30). This actor plays only voices,
// so scaling it cannot drag anything else along.
constexpr std::uint32_t kVoiceHandles[] = {0xF8, 0x104};

// Racedata bounds its player array at 12 (Racedata+0x24 loop bounds); the kart count can never
// exceed the grid.
constexpr int kMaxKarts = 12;

// Above 1 is a deliberate boost: +0xA4 has no clamp of its own and the end product saturates
// safely at Voice::SetVolume (0x800AA860), so a boosted base still wins against the distance and
// fade factors. Capped at the knob's own ceiling (200%) so a corrupt read can never explode.
constexpr float kMaxInitialVolume = 2.0f;

float DesiredVolume(float base, float gain) {
    return std::clamp(base * gain, 0.0f, kMaxInitialVolume);
}

}  // namespace

void KartVolume::Reset() {
    mSlots.clear();
}

void KartVolume::Tick(const RaceState& state) {
    if (!state.valid) {
        // The sounds are gone with the race; the game restamps every base on the next one.
        if (!mSlots.empty()) {
            mSlots.clear();
        }
        return;
    }

    const float playerGain =
        static_cast<float>(RuntimeConfigFile::AccessibilityKartVolume()) / 100.0f;
    const float rivalGain =
        static_cast<float>(RuntimeConfigFile::AccessibilityRivalKartVolume()) / 100.0f;

    const auto scaleSound = [this](std::uint32_t sound, float gain) {
        float current = 0.0f;
        if (!TryFloat(sound + kSoundInitialVolume, current)) {
            return;
        }
        Slot& slot = mSlots[sound];
        // A value this layer did not write means the game restarted the sound: new base.
        if (!slot.known || current != slot.applied) {
            slot.base = current;
            slot.known = true;
        }
        const float desired = DesiredVolume(slot.base, gain);
        if (current == desired) {
            slot.applied = desired;
        } else if (TryWriteFloat(sound + kSoundInitialVolume, desired)) {
            slot.applied = desired;
        }
    };

    std::uint32_t karts[kMaxKarts];
    const int count = ReadKartObjects(karts, kMaxKarts);
    for (int i = 0; i < count; ++i) {
        const float gain = karts[i] == state.kartObject ? playerGain : rivalGain;

        std::uint32_t actor = 0;
        if (TryPointer(karts[i] + kPlayerAudioActor, actor)) {
            for (const std::uint32_t slotOffset : kActorSoundSlots) {
                std::uint32_t sound = 0;
                if (TryPointer(actor + slotOffset, sound)) {
                    scaleSound(sound, gain);
                }
            }
        }

        // The voice lines follow the same knob as the kart they belong to.
        std::uint32_t driver = 0;
        std::uint32_t voiceActor = 0;
        if (TryPointer(karts[i] + kPlayerDriverController, driver) &&
            TryPointer(driver + kDriverCharacterActor, voiceActor)) {
            for (const std::uint32_t handleOffset : kVoiceHandles) {
                std::uint32_t sound = 0;
                if (TryPointer(voiceActor + handleOffset, sound)) {
                    scaleSound(sound, gain);
                }
            }
        }
    }
}

}  // namespace a11y::race
