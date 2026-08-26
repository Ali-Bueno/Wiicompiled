#include "screen_watcher.h"

#include <unordered_map>
#include <vector>

#include "accessibility/a11y_log.h"
#include "accessibility/screen_reader.h"
#include "focus.h"
#include "lyt_walk.h"
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

// The message-window control classes, by vtable: MessageWindowControl (ctor func_805F9700),
// MessageWindowControlScaleFade (func_805F9820) and SimpleMessageWindowControl (func_805F9900).
// Recognising these is what tells a dialog from a menu without guessing from screen names or layer
// bookkeeping: a dialog has a message window, a menu does not.
constexpr std::uint32_t kMessageWindowVtables[] = {0x808B9EE0, 0x808B9EA4, 0x808B9E68};

// Text-only control classes, by vtable. Their constructors are inlined into the page constructors,
// so these were recovered from the relocated vtables in StaticR - the same method that reproduces
// the message-window values above exactly.
//
// The screen's own heading: CtrlMenuPageTitleText, CtrlMenuPressStart (the title screen's "Press
// the A Button") and the CtrlMenuObi banners. Said once, on arrival.
constexpr std::uint32_t kTitleVtables[] = {0x808D36D4, 0x808D3798, 0x808D365C, 0x808D3620};

// CtrlMenuInstructionText is not part of the heading: it describes the option under the cursor and
// changes as the cursor moves. It belongs to the item, which is why the spec orders an item as name,
// then state, then description - reading it with the title mixed a tooltip into the screen name.
constexpr std::uint32_t kTooltipVtables[] = {0x808D3698};

// What each control was showing when the cursor last moved. Some screens - character and kart
// selection - keep the name of the highlighted item in a control of their own rather than in the
// button, so the focused button reads as empty and the name simply changes somewhere else on the
// page. Diffing catches that without knowing which screen is which.
std::unordered_map<std::uint32_t, std::string> g_controlTextSnapshot;
std::uint32_t g_lastFocused = 0;

std::uint32_t g_announcedPage = 0;
// The focused item as last spoken, tracked apart from the whole announcement. Comparing the focus
// against the full sentence made every screen announcement immediately followed by the focused item
// on its own, which cut the message off mid-word.
std::string g_spokenFocusText;

std::uint32_t TopPage() noexcept {
    std::uint32_t manager = 0;
    std::uint32_t section = 0;
    std::uint32_t count = 0;
    if (!Memory::TryRead32(kSectionMgrInstance, manager) || manager == 0 ||
        !Memory::TryRead32(manager + kSectionMgrSection, section) || section == 0 ||
        !Memory::TryRead32(section + kSectionLayerCount, count) || count == 0 ||
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

bool HasVtableIn(std::uint32_t control, const std::uint32_t* known, std::size_t count) noexcept {
    std::uint32_t vtable = 0;
    if (!Memory::TryRead32(control, vtable) || vtable == 0) {
        return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (vtable == known[i]) {
            return true;
        }
    }
    return false;
}

bool IsMessageWindow(std::uint32_t control) noexcept {
    return HasVtableIn(control, kMessageWindowVtables, std::size(kMessageWindowVtables));
}

bool IsTitle(std::uint32_t control) noexcept {
    return HasVtableIn(control, kTitleVtables, std::size(kTitleVtables));
}

bool IsTooltip(std::uint32_t control) noexcept {
    return HasVtableIn(control, kTooltipVtables, std::size(kTooltipVtables));
}

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

// Every control on the page, including those a container built in its own group.
std::vector<std::uint32_t> PageControls(std::uint32_t page) noexcept {
    std::vector<std::uint32_t> controls;
    CollectGroup(page + kPageControlGroup, controls);
    for (std::size_t i = 0; i < controls.size() && controls.size() < kMaxControlsVisited; ++i) {
        CollectGroup(controls[i] + kControlChildGroup, controls);
    }
    return controls;
}

void Append(std::string& into, const std::string& text) {
    if (text.empty() || into.find(text) != std::string::npos) {
        return;
    }
    if (!into.empty()) {
        into += ". ";
    }
    into += text;
}

// What the screen calls itself, plus a dialog's message. Said once, on arrival.
std::string ReadScreenTitle(std::uint32_t page) noexcept {
    std::string result;
    for (const std::uint32_t control : PageControls(page)) {
        if (IsTitle(control) || IsMessageWindow(control)) {
            Append(result, ReadControlText(control));
        }
    }
    return result;
}

// The description of whatever the cursor is on. Said with the item, every time it moves.
std::string ReadTooltip(std::uint32_t page) noexcept {
    std::string result;
    for (const std::uint32_t control : PageControls(page)) {
        if (IsTooltip(control)) {
            Append(result, ReadControlText(control));
        }
    }
    return result;
}

// Refreshes the snapshot and returns whatever text on the page is new since it was last taken.
//
// This is what makes character and kart selection speak. Their buttons hold an id, not a name: the
// page paints the highlighted item's name into a control of its own. So the focused button reads as
// empty while a label elsewhere changes, and the change is the announcement.
std::string RefreshSnapshotAndCollectChanges(std::uint32_t page, bool reportChanges) noexcept {
    std::string changed;
    for (const std::uint32_t control : PageControls(page)) {
        const std::string text = ReadControlText(control);
        if (text.empty()) {
            continue;
        }
        auto& stored = g_controlTextSnapshot[control];
        if (reportChanges && stored != text) {
            Append(changed, text);
        }
        stored = text;
    }
    return changed;
}

}  // namespace

void TickScreenWatcher() {
    const std::uint32_t page = TopPage();
    if (page == 0) {
        return;
    }
    std::uint32_t state = 0;
    if (!Memory::TryRead32(page + kPageState, state) || state != kPageStateActive) {
        return;  // Still animating in or out; nothing stable to read yet.
    }

    const std::uint32_t focused = FocusedControl(page);
    const std::string focusedText = focused != 0 ? ReadControlText(focused) : std::string{};

    if (page != g_announcedPage) {
        g_announcedPage = page;
        g_lastFocused = focused;
        g_controlTextSnapshot.clear();
        RefreshSnapshotAndCollectChanges(page, /*reportChanges=*/false);

        std::string announcement = ReadScreenTitle(page);
        Append(announcement, focusedText);
        Append(announcement, ReadTooltip(page));
        // Remember the focused item, not the whole sentence. Comparing the focus against the full
        // announcement made the next frame say the item again on its own, cutting the message off
        // mid-word - which is what "the OK interrupted the warning" was.
        g_spokenFocusText = focusedText;
        if (announcement.empty()) {
            RT_LOGF(RT_TAG_A11Y, "page %08x settled with nothing readable\n", page);
            return;
        }
        ScreenReader::Instance().Speak(announcement, /*interrupt=*/true);
        return;
    }

    if (focused == g_lastFocused && focusedText == g_spokenFocusText) {
        return;
    }
    g_lastFocused = focused;
    g_spokenFocusText = focusedText;

    // A button that names itself is the common case. When it does not, the screen keeps the name of
    // the highlighted item elsewhere, and what changed on the page is the answer.
    const std::string changed =
        RefreshSnapshotAndCollectChanges(page, /*reportChanges=*/focusedText.empty());
    std::string announcement = focusedText.empty() ? changed : focusedText;
    Append(announcement, ReadTooltip(page));
    if (!announcement.empty()) {
        ScreenReader::Instance().Speak(announcement, /*interrupt=*/true);
    }
}

}  // namespace a11y::ui
