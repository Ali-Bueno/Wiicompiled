#ifndef MKW_ACCESSIBILITY_UI_PAGE_CONTROLS_H
#define MKW_ACCESSIBILITY_UI_PAGE_CONTROLS_H

#include <cstdint>
#include <vector>

namespace a11y::ui {

// Every UIControl a page owns, in the order the page built them, including the ones a container
// built in its own ControlGroup rather than the page's.
//
// This is the whole population of a screen. Partitioning it against SelectableControls() is what
// tells a label from a button without knowing either one's class - see screen_watcher.cpp.
std::vector<std::uint32_t> PageControls(std::uint32_t page) noexcept;

}  // namespace a11y::ui

#endif  // MKW_ACCESSIBILITY_UI_PAGE_CONTROLS_H
