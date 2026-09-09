#include "accessibility/race/item_inventory.h"

#include "accessibility/race/guest_read.h"

namespace a11y::race {
namespace {

// Item::Manager::CreateInstance (0x80799138) stores the instance here.
constexpr std::uint32_t kItemManagerPtr = 0x809C3618;
constexpr std::uint32_t kItemPlayerArray = 0x14;
constexpr std::uint32_t kItemPlayerStride = 584;  // sizeof(Item::Player), from Item::Manager's walk
constexpr std::uint32_t kItemPlayerCountPtr = 0x809C38B8;  // u8
// Item::PlayerInventory sits at Item::Player+0x88 (Item::Player::Init); SetItem (0x807BC940)
// writes the item at inventory+4. It is written only when the roulette settles, never while it
// spins - the cycling display lives in Item::PlayerRoulette, not here.
constexpr std::uint32_t kItemPlayerHeldItem = 0x8C;

// Racedata::GetPlayerIdOfLocalPlayer, the same chain the race record uses.
constexpr std::uint32_t kRacedataPtr = 0x809BD728;
constexpr std::uint32_t kRacedataLocalToPlayer = 0xB84;

// Indexed by the game's item id: the order of the behaviour table Item::InitAllBehavior
// (0x807BCAE0) fills at 0x809C36A0, whose per-id object family and count name each entry
// (id 3 is family FIB, 5 is Kinoko x3, 14 is Kumo, 16-18 are the x3 trails).
constexpr const char* kItemKeys[] = {
    "item_green_shell",   "item_red_shell",         "item_banana",
    "item_fake_item_box", "item_mushroom",          "item_triple_mushrooms",
    "item_bob_omb",       "item_spiny_shell",       "item_lightning",
    "item_star",          "item_golden_mushroom",   "item_mega_mushroom",
    "item_blooper",       "item_pow_block",         "item_thunder_cloud",
    "item_bullet_bill",   "item_triple_green_shells", "item_triple_red_shells",
    "item_triple_bananas",
};

}  // namespace

bool TryReadHeldItem(std::uint32_t& item) {
    std::uint32_t racedata = 0;
    std::uint32_t manager = 0;
    std::uint8_t rawId = 0;
    std::uint8_t players = 0;
    if (!TryPointer(kRacedataPtr, racedata) || !TryPointer(kItemManagerPtr, manager) ||
        !TryU8(racedata + kRacedataLocalToPlayer, rawId) ||
        !TryU8(kItemPlayerCountPtr, players)) {
        return false;
    }
    const int id = static_cast<std::int8_t>(rawId);
    if (id < 0 || id >= static_cast<int>(players)) {
        return false;
    }
    std::uint32_t array = 0;
    return TryPointer(manager + kItemPlayerArray, array) &&
           Memory::TryRead32(array + static_cast<std::uint32_t>(id) * kItemPlayerStride +
                                 kItemPlayerHeldItem,
                             item);
}

const char* ItemNameKey(std::uint32_t item) {
    constexpr std::uint32_t kCount = sizeof(kItemKeys) / sizeof(kItemKeys[0]);
    return item < kCount ? kItemKeys[item] : nullptr;
}

}  // namespace a11y::race
