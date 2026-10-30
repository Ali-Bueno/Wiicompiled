#ifndef MKW_ACCESSIBILITY_MENU_ROWS_CONTROLS_H
#define MKW_ACCESSIBILITY_MENU_ROWS_CONTROLS_H

#include <cstdint>
#include <string>
#include <vector>

#include "accessibility/menu/menu_tree.h"

// Pieces of the Controls submenu that live in their own files.

namespace a11y::menu {

// The game port (0-3) the port row selected; every controller row acts on it.
uint32_t ControllerPort();
// Interrupting confirmation for rows that have no value of their own to re-speak.
void SayNow(const std::string& text);

std::vector<MenuRow> PresetRows();
std::vector<MenuRow> WiiRemoteRows();

}  // namespace a11y::menu

#endif  // MKW_ACCESSIBILITY_MENU_ROWS_CONTROLS_H
