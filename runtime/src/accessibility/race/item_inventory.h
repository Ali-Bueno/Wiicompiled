#ifndef MKW_ACCESSIBILITY_RACE_ITEM_INVENTORY_H
#define MKW_ACCESSIBILITY_RACE_ITEM_INVENTORY_H

#include <cstdint>

namespace a11y::race {

// Item::Player::Init writes this as the empty inventory and Item::Player::DecideItem only rolls a
// new item when the held type reads back as it, which is what makes it the empty-handed value.
inline constexpr std::uint32_t kItemNone = 20;

// The item the local human is holding, as the game's own item id. False when no race is loaded
// or the chain could not be read; a caller must then say nothing rather than assume.
bool TryReadHeldItem(std::uint32_t& item);

// Localization key naming an item id, or nullptr for an id the game never hands out.
const char* ItemNameKey(std::uint32_t item);

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_ITEM_INVENTORY_H
