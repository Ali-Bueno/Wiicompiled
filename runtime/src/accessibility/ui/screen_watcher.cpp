#include "screen_watcher.h"

#include "accessibility/a11y_log.h"
#include "accessibility/screen_reader.h"
#include "lyt_walk.h"
#include "memory.h"

namespace a11y::ui {
namespace {

// SectionMgr::CreateInstance (func_80634C90) stores the singleton here, and SectionMgr::MenuUpdate
// (func_8063583C) reads the Section from its offset 0.
constexpr std::uint32_t kSectionMgrInstance = 0x809C1E38;
constexpr std::uint32_t kSectionMgrSection = 0x00;

// Section::GetTopLayerPage (func_80622EA0): a count at +0x37C and an array of Page* at +0x354, the
// last entry being the page on top. Section::AddPageLayerAnimated (func_80623228) writes them.
constexpr std::uint32_t kSectionLayerArray = 0x354;
constexpr std::uint32_t kSectionLayerCount = 0x37C;
constexpr std::uint32_t kMaxLayers = 16;

// Page state, from Page::UpdateState (func_80601D24) and Section::UpdateLayers (func_80623068):
// 1 initialised, 2 activated, 3 entering, 4 active, 5 exiting, 6 finished. Waiting for 4 is what
// replaces guessing an animation length - by then the entrance has finished, the page's initial
// focus is set, and pane alpha is meaningful again.
constexpr std::uint32_t kPageState = 0x08;
constexpr std::uint32_t kPageStateActive = 4;

// Page layout, from Page::InitControlGroup (func_8060245C) and Page::AddControl (func_8060246C).
constexpr std::uint32_t kPageControlGroup = 0x24;

// ControlGroup, from ControlGroup::Init (func_805C2620) and InitControls (func_805C2868).
constexpr std::uint32_t kControlGroupArray = 0x00;
constexpr std::uint32_t kControlGroupCount = 0x10;

// Real pages hold single digits worth of controls; this only guards a half-initialised group.
constexpr std::uint32_t kMaxControlsPerPage = 64;

std::uint32_t g_announcedPage = 0;
std::string g_focusedText;
std::string g_spokenFocusedText;
// Which screen the focused item belongs to. Without this, focus recorded on the page being left
// leaks into the announcement of the page being entered - "press the A button" then "NEW".
std::uint32_t g_focusedPage = 0;

std::uint32_t TopPage() noexcept {
    std::uint32_t manager = 0;
    if (!Memory::TryRead32(kSectionMgrInstance, manager) || manager == 0) {
        return 0;
    }
    std::uint32_t section = 0;
    if (!Memory::TryRead32(manager + kSectionMgrSection, section) || section == 0) {
        return 0;
    }
    std::uint32_t count = 0;
    if (!Memory::TryRead32(section + kSectionLayerCount, count) || count == 0 ||
        count > kMaxLayers) {
        return 0;
    }
    std::uint32_t page = 0;
    if (!Memory::TryRead32(section + kSectionLayerArray + (count - 1) * sizeof(std::uint32_t),
                           page)) {
        return 0;
    }
    return page;
}

// Whether a page is still somewhere in the section's layer stack.
bool IsPageInStack(std::uint32_t page) noexcept {
    std::uint32_t manager = 0;
    std::uint32_t section = 0;
    std::uint32_t count = 0;
    if (page == 0 || !Memory::TryRead32(kSectionMgrInstance, manager) || manager == 0 ||
        !Memory::TryRead32(manager + kSectionMgrSection, section) || section == 0 ||
        !Memory::TryRead32(section + kSectionLayerCount, count) || count == 0 ||
        count > kMaxLayers) {
        return false;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t layer = 0;
        if (Memory::TryRead32(section + kSectionLayerArray + i * sizeof(std::uint32_t), layer) &&
            layer == page) {
            return true;
        }
    }
    return false;
}

// The screen's non-button text: its title, and a dialog's message. Buttons are left out because the
// focused one is spoken separately and reciting the rest on arrival is exhausting to listen to.
std::string ReadPageLabels(std::uint32_t page) noexcept {
    const std::uint32_t group = page + kPageControlGroup;
    std::uint32_t count = 0;
    std::uint32_t array = 0;
    if (!Memory::TryRead32(group + kControlGroupCount, count) || count == 0 ||
        count > kMaxControlsPerPage) {
        return {};
    }
    if (!Memory::TryRead32(group + kControlGroupArray, array) || array == 0) {
        return {};
    }

    std::string result;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t control = 0;
        if (!Memory::TryRead32(array + i * sizeof(std::uint32_t), control) || control == 0) {
            continue;
        }
        if (IsKnownPushButton(control)) {
            continue;
        }
        const std::string text = ReadControlText(control);
        if (text.empty() || result.find(text) != std::string::npos) {
            continue;
        }
        if (!result.empty()) {
            result += ". ";
        }
        result += text;
    }
    return result;
}

}  // namespace

void NoteFocusedControl(std::uint32_t control, std::string text) {
    // Anything that takes focus is a button by definition, which is how the page reader tells a
    // button from a title without knowing anything about the screen.
    NotePushButtonInstance(control);
    g_focusedText = std::move(text);
    g_focusedPage = TopPage();
}

void TickScreenWatcher() {
    const std::uint32_t page = TopPage();
    if (page == 0) {
        return;
    }
    std::uint32_t state = 0;
    if (!Memory::TryRead32(page + kPageState, state) || state != kPageStateActive) {
        return;  // Still animating in or out; nothing stable to read yet.
    }

    if (page != g_announcedPage) {
        // A dialog is pushed on top of the screen it interrupts, so the page we were just on is
        // still in the stack underneath. Navigating to another screen replaces it instead. That
        // difference is the whole test: a dialog's message has to be read, a menu's title does not
        // - the player asked for the focused option alone when entering a menu.
        const bool isOverlay = IsPageInStack(g_announcedPage);
        g_announcedPage = page;
        std::string announcement = isOverlay ? ReadPageLabels(page) : std::string{};
        if (g_focusedPage != page) {
            // Focus belongs to the screen we just left.
            g_focusedText.clear();
        }
        if (!g_focusedText.empty()) {
            if (!announcement.empty()) {
                announcement += ". ";
            }
            announcement += g_focusedText;
        }
        g_spokenFocusedText = g_focusedText;
        if (!announcement.empty()) {
            ScreenReader::Instance().Speak(announcement, /*interrupt=*/true);
        } else {
            RT_LOGF(RT_TAG_A11Y, "page %08x settled with nothing readable\n", page);
        }
        return;
    }

    if (g_focusedText != g_spokenFocusedText) {
        g_spokenFocusedText = g_focusedText;
        if (!g_focusedText.empty()) {
            ScreenReader::Instance().Speak(g_focusedText, /*interrupt=*/true);
        }
    }
}

}  // namespace a11y::ui
