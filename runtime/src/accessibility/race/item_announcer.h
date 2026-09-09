#ifndef MKW_ACCESSIBILITY_RACE_ITEM_ANNOUNCER_H
#define MKW_ACCESSIBILITY_RACE_ITEM_ANNOUNCER_H

#include <cstdint>

#include "accessibility/race/item_inventory.h"

namespace a11y::race {

struct RaceState;

// Speaks the item the roulette settled on, once, on the frame the game hands it over - the same
// frame a sighted player sees it. Losing or using an item is left to the game's own sounds.
class ItemAnnouncer {
public:
    void Reset();
    void Tick(const RaceState& state);

private:
    std::uint32_t mLast = kItemNone;
    bool mArmed = false;
};

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_ITEM_ANNOUNCER_H
