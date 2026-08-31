#ifndef MKW_ACCESSIBILITY_UI_ENTITY_INFO_H
#define MKW_ACCESSIBILITY_UI_ENTITY_INFO_H

#include <cstdint>
#include <string>
#include <vector>

namespace a11y::ui {

// What a select screen shows about the driver or vehicle under the cursor besides its name: the
// weight class, and on the vehicle screens the seven stat bars.
//
// This is the one thing the pane walk cannot do. Everything else the reader speaks is text it found
// on a TextBox; these are drawn as pictures, so there is no string anywhere to find and the numbers
// have to come from the game's own data instead.
//
// Empty for any other focused control, which is every other screen.
std::string DescribeFocusedEntity(const std::vector<std::uint32_t>& layers, std::uint32_t focused);

}  // namespace a11y::ui

#endif  // MKW_ACCESSIBILITY_UI_ENTITY_INFO_H
