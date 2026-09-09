#include "accessibility/race/item_announcer.h"

#include "accessibility/localization.h"
#include "accessibility/race/race_state.h"
#include "accessibility/screen_reader.h"

namespace a11y::race {

void ItemAnnouncer::Reset() {
    mLast = kItemNone;
    mArmed = false;
}

void ItemAnnouncer::Tick(const RaceState& state) {
    std::uint32_t held = 0;
    if (!state.valid || !TryReadHeldItem(held)) {
        Reset();
        return;
    }
    // The first readable frame only records what is already there, so a race joined mid-hold
    // (or a frame the read skipped) never re-announces an item the player already heard.
    if (!mArmed) {
        mLast = held;
        mArmed = true;
        return;
    }
    if (held == mLast) {
        return;
    }
    mLast = held;
    const char* key = held == kItemNone ? nullptr : ItemNameKey(held);
    if (key) {
        ScreenReader::Instance().Speak(loc::Get(key), /*interrupt=*/false);
    }
}

}  // namespace a11y::race
