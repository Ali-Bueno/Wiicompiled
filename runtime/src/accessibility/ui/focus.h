#ifndef MKW_ACCESSIBILITY_UI_FOCUS_H
#define MKW_ACCESSIBILITY_UI_FOCUS_H

#include <cstdint>

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

}  // namespace a11y::ui

#endif  // MKW_ACCESSIBILITY_UI_FOCUS_H
