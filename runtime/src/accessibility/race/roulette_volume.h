#ifndef MKW_ACCESSIBILITY_RACE_ROULETTE_VOLUME_H
#define MKW_ACCESSIBILITY_RACE_ROULETTE_VOLUME_H

#include <cstdint>

namespace a11y::race {

struct RaceState;

// Scales the item roulette's spin sound (and the "item decided" jingle) so it stops masking the
// engine. The sounds sit on two global UI SoundHandles the game re-uses for menu clicks, so each
// is scaled only while it carries the roulette's own sound ids.
class RouletteVolume {
public:
    void Tick(const RaceState& state);
    void Reset();

private:
    // Same re-base pattern as KartVolume, per handle: the base is the game's own value, re-read
    // whenever the sound object or its observed volume is not what this layer last wrote.
    struct Tracked {
        std::uint32_t sound = 0;
        float base = 1.0f;
        float applied = 1.0f;
        bool known = false;
    };

    void Scale(std::uint32_t handleAddr, bool oneShotIds, float gain, Tracked& tracked);

    Tracked mHeld;
    Tracked mOneShot;
};

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_ROULETTE_VOLUME_H
