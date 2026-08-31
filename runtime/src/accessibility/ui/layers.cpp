#include "layers.h"

#include <cstddef>

#include "memory.h"

namespace a11y::ui {
namespace {

// SectionMgr::CreateInstance (func_80634C90) stores the singleton here, and SectionMgr::MenuUpdate
// (func_8063583C) reads the Section from its offset 0.
constexpr std::uint32_t kSectionMgrInstance = 0x809C1E38;
constexpr std::uint32_t kSectionMgrSection = 0x00;

// Section::GetTopLayerPage (func_80622EA0): a count at +0x37C and an array of Page* at +0x354, the
// last entry being the page on top.
constexpr std::uint32_t kSectionLayerArray = 0x354;
constexpr std::uint32_t kSectionLayerCount = 0x37C;
constexpr std::uint32_t kMaxLayers = 16;

// Page state, from Page::UpdateState (func_80601D24): 1 initialised, 2 activated, 3 entering,
// 4 active, 5 exiting, 6 finished. Waiting for 4 is what replaces guessing an animation length -
// by then the entrance has finished, the initial focus is set, and pane alpha means something.
constexpr std::uint32_t kPageState = 0x08;
constexpr std::uint32_t kPageStateActive = 4;

bool IsSettled(std::uint32_t page) noexcept {
    std::uint32_t state = 0;
    return Memory::TryRead32(page + kPageState, state) && state == kPageStateActive;
}

std::uint32_t LayerAt(std::uint32_t section, std::uint32_t index) noexcept {
    std::uint32_t page = 0;
    if (!Memory::TryRead32(section + kSectionLayerArray + index * sizeof(std::uint32_t), page)) {
        return 0;
    }
    return page;
}

}  // namespace

std::uint32_t SectionManager() noexcept {
    std::uint32_t manager = 0;
    return Memory::TryRead32(kSectionMgrInstance, manager) ? manager : 0;
}

std::vector<std::uint32_t> ActiveLayerPages() noexcept {
    std::vector<std::uint32_t> pages;
    const std::uint32_t manager = SectionManager();
    std::uint32_t section = 0;
    std::uint32_t count = 0;
    if (manager == 0 || !Memory::TryRead32(manager + kSectionMgrSection, section) || section == 0 ||
        !Memory::TryRead32(section + kSectionLayerCount, count) || count == 0 ||
        count > kMaxLayers) {
        return pages;
    }

    // Gated on the top layer, not on each one: a lower layer settled long ago, and the question the
    // watcher is asking is whether the screen the player is on has stopped moving.
    const std::uint32_t top = LayerAt(section, count - 1);
    if (top == 0 || !IsSettled(top)) {
        return pages;
    }

    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t page = LayerAt(section, i);
        if (page != 0 && IsSettled(page)) {
            pages.push_back(page);
        }
    }
    return pages;
}

}  // namespace a11y::ui
