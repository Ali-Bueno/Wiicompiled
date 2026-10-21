#ifndef MKW_ACCESSIBILITY_RACE_KART_VOLUME_H
#define MKW_ACCESSIBILITY_RACE_KART_VOLUME_H

namespace a11y::race {

struct RaceState;

// Scales the player's and the rivals' kart sounds separately, so a blind player can pull the
// rival engines under their own - the one that carries the steering guide. Writes the volume of
// each kart's own nw4r sound actors every frame; nothing is hooked and nothing is synthesized,
// the game's mixer keeps doing the mixing.
class KartVolume {
public:
    void Tick(const RaceState& state);
};

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_KART_VOLUME_H
