#ifndef MKW_ACCESSIBILITY_UI_FOCUS_H
#define MKW_ACCESSIBILITY_UI_FOCUS_H

#include <cstdint>
#include <vector>

namespace a11y::ui {

// Which control the player's cursor is on, read straight out of the page's manipulator manager.
//
// This deliberately replaces listening for selection events. PushButton::HandleSelect only ever
// covers plain buttons, which left character, kart and Mii selection silent, and adding a hook per
// control class would leave every screen nobody thought of mute. The manipulator is the layer every
// control type goes through, so reading it answers the question once, for all of them.
//
// Returns 0 when nothing is focused.
std::uint32_t FocusedControl(std::uint32_t page) noexcept;

// Every control on the page the cursor can reach, from the same manipulator manager - one entry per
// manipulator the page registered.
//
// This is the structural half of telling a button from a label: the game itself decides what is
// selectable, so anything else on the page is text it is showing. Asking the game beats keeping a
// list of known text classes, which leaves any class not on it unread.
//
// Returns empty when the page has nothing selectable, and also when the list could not be read -
// callers must not take an empty result as proof that a page is all labels. FocusedControl()
// returning non-zero while this returns empty means the read failed.
std::vector<std::uint32_t> SelectableControls(std::uint32_t page) noexcept;

}  // namespace a11y::ui

#endif  // MKW_ACCESSIBILITY_UI_FOCUS_H
