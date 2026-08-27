#include "accessibility/race/roulette_volume.h"

#include <algorithm>

#include "accessibility/race/guest_read.h"
#include "accessibility/race/nw4r_sound.h"
#include "accessibility/race/race_state.h"
#include "runtime_config.h"

namespace a11y::race {
namespace {

// Audio::RSARPlayer's two global UI SoundHandles (a SoundHandle is just the BasicSound*).
// func_8071469C / func_80714690 return their addresses; the roulette driver func_807156F4
// (called per frame by Audio::RaceRSARPlayer::Calc 0x8071646C) holds the spin on the first and
// writes its pan there, which is what proves the sound lives on this handle.
constexpr std::uint32_t kHeldUiSoundHandle = 0x809C283C;
constexpr std::uint32_t kOneShotUiSoundHandle = 0x809C282C;

// Sound archive ids, passed unremapped down to BasicSound::SetId: 226 is the spin held every
// frame by func_807156F4; 227/228 the "item decided" jingle (normal / thundercloud) fired by
// Item::Player::Update (0x80797928). The id filter is what keeps menu clicks - which share these
// handles through UIControl::HoldSound - untouched.
constexpr std::uint32_t kRouletteSpinId = 226;
constexpr std::uint32_t kItemDecidedId = 227;
constexpr std::uint32_t kItemDecidedCloudId = 228;

}  // namespace

void RouletteVolume::Reset() {
    mHeld = Tracked{};
    mOneShot = Tracked{};
}

void RouletteVolume::Scale(std::uint32_t handleAddr, bool oneShotIds, float gain,
                           Tracked& tracked) {
    std::uint32_t sound = 0;
    std::uint32_t id = 0;
    float current = 0.0f;
    if (!TryPointer(handleAddr, sound) || !Memory::TryRead32(sound + kSoundIdOffset, id) ||
        (oneShotIds ? (id != kItemDecidedId && id != kItemDecidedCloudId)
                    : id != kRouletteSpinId) ||
        !TryFloat(sound + kSoundInitialVolume, current)) {
        tracked.known = false;  // gone or not ours: forget the base
        return;
    }
    if (sound != tracked.sound || !tracked.known || current != tracked.applied) {
        tracked.sound = sound;
        tracked.base = current;
        tracked.known = true;
    }
    const float desired = std::clamp(tracked.base * gain, 0.0f, 1.0f);
    if (current == desired) {
        tracked.applied = desired;
    } else if (TryWriteFloat(sound + kSoundInitialVolume, desired)) {
        tracked.applied = desired;
    }
}

void RouletteVolume::Tick(const RaceState& state) {
    // The race gate also keeps the shared handles alone in the menus, on top of the id filter.
    if (!state.valid) {
        Reset();
        return;
    }
    const float gain =
        static_cast<float>(RuntimeConfigFile::AccessibilityItemRouletteVolume()) / 100.0f;
    Scale(kHeldUiSoundHandle, /*oneShotIds=*/false, gain, mHeld);
    Scale(kOneShotUiSoundHandle, /*oneShotIds=*/true, gain, mOneShot);
}

}  // namespace a11y::race
