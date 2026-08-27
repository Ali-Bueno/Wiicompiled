#ifndef MKW_ACCESSIBILITY_RACE_KART_VOLUME_H
#define MKW_ACCESSIBILITY_RACE_KART_VOLUME_H

#include <cstdint>
#include <unordered_map>

namespace a11y::race {

struct RaceState;

// Scales the player's and the rivals' kart sounds separately, so a blind player can pull the
// rival engines under their own - the one that carries the steering guide. Writes the initial
// volume factor on each kart's own nw4r sounds every frame; nothing is hooked and nothing is
// synthesized, the game's mixer keeps doing the mixing.
class KartVolume {
public:
    void Tick(const RaceState& state);
    void Reset();

private:
    // The game's own value the multiplier is applied against. Re-based whenever the observed
    // value is not the one last written here, which is how a pooled sound restarting under the
    // same address is caught - the same pattern MusicAttenuation uses on the sound players.
    struct Slot {
        float base = 1.0f;
        float applied = 1.0f;
        bool known = false;
    };

    std::unordered_map<std::uint32_t, Slot> mSlots;
};

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_KART_VOLUME_H
