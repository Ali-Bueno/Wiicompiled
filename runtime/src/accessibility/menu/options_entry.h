#ifndef MKW_ACCESSIBILITY_MENU_OPTIONS_ENTRY_H
#define MKW_ACCESSIBILITY_MENU_OPTIONS_ENTRY_H

#include <cstdint>

struct CpuContext;

// The button the mod adds to the game's own Options page, which opens the settings menu. The
// same code serves Base and Retro Rewind; see docs/options-entry.md.
namespace a11y::menu {

// Before ControlGroup::InitControls: an Options page's group grows by our button.
void OnControlsInitStarting(CpuContext* ctx, std::uint32_t group) noexcept;
// After it: the button gets its label.
void OnControlsInitDone(CpuContext* ctx, std::uint32_t group) noexcept;
void OnButtonClicked(std::uint32_t button) noexcept;
// Before Pages::Options::__dt, the way a derived page destroys its own members first.
void OnOptionsDestroying(CpuContext* ctx, std::uint32_t page) noexcept;

}  // namespace a11y::menu

#endif  // MKW_ACCESSIBILITY_MENU_OPTIONS_ENTRY_H
