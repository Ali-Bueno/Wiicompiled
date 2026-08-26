#include "page_reader.h"

#include "accessibility/a11y_log.h"
#include "accessibility/screen_reader.h"
#include "lyt_walk.h"
#include "memory.h"

namespace a11y::ui {
namespace {

// Page layout, from Page::InitControlGroup (func_8060245C) and Page::AddControl (func_8060246C),
// which both operate on this+0x24; Page::Activate (func_80601AEC) reads the manipulator manager
// from this+0x38.
constexpr std::uint32_t kPageControlGroup = 0x24;
constexpr std::uint32_t kPageManipulatorManager = 0x38;

// ControlGroup, from ControlGroup::Init (func_805C2620) and InitControls (func_805C2868):
// a flat array of UIControl* and the count that bounds it.
constexpr std::uint32_t kControlGroupArray = 0x00;
constexpr std::uint32_t kControlGroupCount = 0x10;

// ControlsManipulatorManager::__ct (func_805F09A8) writes this vtable; PageManipulatorManager
// (func_805EF240) writes 0x808B9A48 instead. Page::Activate distinguishes them by RTTI to decide
// whether to set up control selection at all, which makes this the game's own answer to "can
// anything here take focus?".
constexpr std::uint32_t kControlsManipulatorManagerVtable = 0x808B99E8;

// A page cannot hold more controls than this; the real counts are single digits. Guards against
// reading a half-initialised group.
constexpr std::uint32_t kMaxControlsPerPage = 64;

// Page::Activate runs before the entrance animation starts, so the read waits for the page to
// settle. Two frames is enough for OnActivate's own work to land; the alpha test is skipped instead
// of waiting for the fade, which would mean guessing an animation length.
constexpr int kFramesBeforeReading = 2;

std::uint32_t g_queuedPage = 0;
int g_framesUntilRead = 0;
std::uint32_t g_lastReadPage = 0;

}  // namespace

bool PageHasFocusableControls(std::uint32_t page) noexcept {
    std::uint32_t manager = 0;
    if (!Memory::TryRead32(page + kPageManipulatorManager, manager) || manager == 0) {
        return false;
    }
    std::uint32_t vtable = 0;
    if (!Memory::TryRead32(manager, vtable)) {
        return false;
    }
    return vtable == kControlsManipulatorManagerVtable;
}

std::string ReadPageText(std::uint32_t page, bool requireOpaque) noexcept {
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
        const std::string text = ReadControlText(control, requireOpaque);
        if (text.empty() || result.find(text) != std::string::npos) {
            continue;
        }
        if (!result.empty()) {
            result += ", ";
        }
        result += text;
    }
    return result;
}

void QueuePageForReading(std::uint32_t page) {
    if (page == 0 || page == g_lastReadPage) {
        return;
    }
    g_queuedPage = page;
    g_framesUntilRead = kFramesBeforeReading;
}

void TickPageReader() {
    if (g_queuedPage == 0) {
        return;
    }
    if (--g_framesUntilRead > 0) {
        return;
    }

    const std::uint32_t page = g_queuedPage;
    g_queuedPage = 0;
    g_lastReadPage = page;

    // A page with selectable controls announces itself through the focus path, one item at a time.
    // Reading the whole screen there would recite every button on entry, which the spec explicitly
    // does not ask for.
    if (PageHasFocusableControls(page)) {
        return;
    }

    // The entrance fade has not finished, so alpha is not a usable signal yet.
    const std::string text = ReadPageText(page, /*requireOpaque=*/false);
    if (text.empty()) {
        RT_LOGF(RT_TAG_A11Y, "page %08x has no focusable control and no readable text\n", page);
        return;
    }
    ScreenReader::Instance().Speak(text, /*interrupt=*/true);
}

}  // namespace a11y::ui
