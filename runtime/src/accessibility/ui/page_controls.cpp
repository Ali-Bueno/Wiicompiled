#include "page_controls.h"

#include <cstddef>

#include "memory.h"

namespace a11y::ui {
namespace {

// A page's own ControlGroup (Page::InitControlGroup, func_8060245C) and a control's nested one
// (UIControl::InitControlGroup, func_8063D268). A grid like character select builds its thirty
// buttons in its own group, not the page's, which is why a page-only walk finds nothing there.
constexpr std::uint32_t kPageControlGroup = 0x24;
constexpr std::uint32_t kControlChildGroup = 104;
constexpr std::uint32_t kControlGroupArray = 0x00;
constexpr std::uint32_t kControlGroupCount = 0x10;

// Real groups hold single digits to low tens of controls; this only guards a half-built one.
constexpr std::uint32_t kMaxControlsPerGroup = 128;
constexpr std::size_t kMaxControlsVisited = 512;

void CollectGroup(std::uint32_t group, std::vector<std::uint32_t>& out) noexcept {
    std::uint32_t count = 0;
    std::uint32_t array = 0;
    if (!Memory::TryRead32(group + kControlGroupCount, count) || count == 0 ||
        count > kMaxControlsPerGroup || !Memory::TryRead32(group + kControlGroupArray, array) ||
        array == 0) {
        return;
    }
    for (std::uint32_t i = 0; i < count && out.size() < kMaxControlsVisited; ++i) {
        std::uint32_t control = 0;
        if (Memory::TryRead32(array + i * sizeof(std::uint32_t), control) && control != 0) {
            out.push_back(control);
        }
    }
}

}  // namespace

std::vector<std::uint32_t> PageControls(std::uint32_t page) noexcept {
    std::vector<std::uint32_t> controls;
    if (page == 0) {
        return controls;
    }
    CollectGroup(page + kPageControlGroup, controls);
    for (std::size_t i = 0; i < controls.size() && controls.size() < kMaxControlsVisited; ++i) {
        CollectGroup(controls[i] + kControlChildGroup, controls);
    }
    return controls;
}

}  // namespace a11y::ui
