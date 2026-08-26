#ifndef MKW_ACCESSIBILITY_UI_EVENTS_H
#define MKW_ACCESSIBILITY_UI_EVENTS_H

#include <cstdint>

namespace a11y::ui {

// What the game's UI told us. The hooks in ui_hooks.cpp do nothing but call these - all logic lives
// behind them, so the hook bodies stay trivial and survive an upstream rebase.
//
// Everything here runs on the guest thread inside a game function, so implementations must be cheap
// and must never throw.

// A control took focus. `initial` distinguishes the default selection made when a page opens from
// the user actually moving the cursor.
void OnControlSelected(std::uint32_t control, bool initial);

// A page finished its entrance animation and is now readable.
void OnPageEntered(std::uint32_t page);

// A page is being activated, before its entrance animation.
void OnPageActivated(std::uint32_t page);

// The section (the game's top-level menu grouping) changed.
void OnSectionEntered(std::uint32_t section);

}  // namespace a11y::ui

#endif  // MKW_ACCESSIBILITY_UI_EVENTS_H
