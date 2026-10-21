#include "accessibility/race/kart_volume.h"

#include <cstdint>

#include "accessibility/race/guest_read.h"
#include "accessibility/race/race_state.h"
#include "runtime_config.h"

namespace a11y::race {
namespace {

// Kart::Player::CreateSound (0x8058ECE0) / Kart::PlayerBike::CreateSound (0x8058F56C) store the
// kart's own Audio::KartActor here. Every kart has one, CPU rivals included - the RaceMgr array
// at +0x18 holds only the up-to-4 local players and is the wrong place to look for rivals.
constexpr std::uint32_t kPlayerAudioActor = 0x38;

// The character VOICES live on a separate per-kart Audio::CharacterActor: Kart::Player::CreateModel
// (0x8058F820) stores the DriverController at +0x18, and DriverController::__ct (0x807C7364) its
// CharacterActor at +0x0C (the chain Kart::Link::GetCharacterActor 0x805907A0 reads).
constexpr std::uint32_t kPlayerDriverController = 0x18;
constexpr std::uint32_t kDriverCharacterActor = 0x0C;

// nw4r::snd::SoundActor's own volume, 1.0 from SoundActor::__ct (0x8009DBE0) and never written by
// the game. BasicSound::Update (0x8008E8D0) copies it into every sound the actor plays before each
// parameter push, the starting one included, so a new sound is scaled from its first sample. Both
// actors above have the SoundActor base at offset 0. See docs/kart-volume.md.
constexpr std::uint32_t kSoundActorVolume = 0x48;

// Racedata bounds its player array at 12 (Racedata+0x24 loop bounds); the kart count can never
// exceed the grid.
constexpr int kMaxKarts = 12;

void ApplyActorVolume(std::uint32_t actor, float volume) {
    float current = 0.0f;
    if (TryFloat(actor + kSoundActorVolume, current) && current != volume) {
        TryWriteFloat(actor + kSoundActorVolume, volume);
    }
}

}  // namespace

void KartVolume::Tick(const RaceState& state) {
    if (!state.valid) {
        return;
    }

    // Above 1 is a deliberate boost: BasicSound::UpdateParam (0x8008EF20) does not clamp the
    // actor factor, only Voice::SetVolume (0x800AA860) clamps the end product.
    const float playerGain =
        static_cast<float>(RuntimeConfigFile::AccessibilityKartVolume()) / 100.0f;
    const float rivalGain =
        static_cast<float>(RuntimeConfigFile::AccessibilityRivalKartVolume()) / 100.0f;

    std::uint32_t karts[kMaxKarts];
    const int count = ReadKartObjects(karts, kMaxKarts);
    for (int i = 0; i < count; ++i) {
        const float gain = karts[i] == state.kartObject ? playerGain : rivalGain;

        std::uint32_t actor = 0;
        if (TryPointer(karts[i] + kPlayerAudioActor, actor)) {
            ApplyActorVolume(actor, gain);
        }

        // The voice lines follow the same knob as the kart they belong to.
        std::uint32_t driver = 0;
        std::uint32_t voiceActor = 0;
        if (TryPointer(karts[i] + kPlayerDriverController, driver) &&
            TryPointer(driver + kDriverCharacterActor, voiceActor)) {
            ApplyActorVolume(voiceActor, gain);
        }
    }
}

}  // namespace a11y::race
