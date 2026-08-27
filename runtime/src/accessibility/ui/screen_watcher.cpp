#include "screen_watcher.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "accessibility/a11y_log.h"
#include "accessibility/screen_reader.h"
#include "focus.h"
#include "lyt_walk.h"
#include "memory.h"
#include "page_controls.h"

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

// Label classes whose text follows the cursor: a tooltip, or the name a grid paints outside its
// buttons. Learned from behaviour - a label seen changing while the cursor moved belongs to the
// item - and used only to order the arrival announcement, so a class nobody has seen before is
// announced as part of the screen rather than going unread. Never a hardcoded vtable; the same
// approach as the TextBox classes in lyt_walk.cpp.
std::unordered_set<std::uint32_t>& ItemBoundClasses() {
    static std::unordered_set<std::uint32_t> classes;
    return classes;
}

std::uint32_t ClassOf(std::uint32_t control) noexcept {
    std::uint32_t vtable = 0;
    return Memory::TryRead32(control, vtable) ? vtable : 0;
}

bool IsItemBound(std::uint32_t control) noexcept {
    const std::uint32_t klass = ClassOf(control);
    return klass != 0 && ItemBoundClasses().count(klass) != 0;
}

void LearnItemBound(std::uint32_t control) {
    const std::uint32_t klass = ClassOf(control);
    if (klass != 0 && ItemBoundClasses().insert(klass).second) {
        RT_LOGF(RT_TAG_A11Y, "label class %08x follows the cursor\n", klass);
    }
}

std::uint32_t g_announcedPage = 0;
std::uint32_t g_lastFocused = 0;
// The focused item as last spoken, tracked apart from the whole announcement. Comparing the focus
// against the full sentence made every screen announcement immediately followed by the focused item
// on its own, which cut the message off mid-word.
std::string g_spokenFocusText = {};
// What each label was showing when the cursor last moved, so a label that follows the cursor can be
// told from one that just sits there.
std::unordered_map<std::uint32_t, std::string> g_labelText;

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

// The screen's text, by exclusion: every control the cursor cannot reach. That is the whole
// classification, and it needs to know nothing about control classes - which is the point. A text
// class nobody has seen before is a label because the cursor cannot reach it, not because it was on
// a list, so it reads instead of going mute.
struct PageLabels {
    std::vector<std::uint32_t> controls;
    bool trustworthy = false;
    bool anySelectable = false;
};

PageLabels ReadLabels(std::uint32_t page, std::uint32_t focused) noexcept {
    PageLabels labels;
    const std::vector<std::uint32_t> selectable = SelectableControls(page);
    labels.anySelectable = !selectable.empty();

    // A page with a focused control necessarily has selectable ones. If none came back, the
    // manipulator list did not read, and calling every button a label would recite whole menus -
    // the one thing the spec forbids outright. Announce only the focused item instead.
    labels.trustworthy = labels.anySelectable || focused == 0;
    if (!labels.trustworthy) {
        return labels;
    }

    std::unordered_set<std::uint32_t> reachable(selectable.begin(), selectable.end());
    if (focused != 0) {
        reachable.insert(focused);  // it resolved, so it is reachable whatever the list said
    }
    for (const std::uint32_t control : PageControls(page)) {
        if (reachable.count(control) == 0) {
            labels.controls.push_back(control);
        }
    }
    return labels;
}

// One sentence out of the parts a screen contributed, in the order they were gathered.
//
// A part already contained in another is dropped, whichever way round they arrived. Screens hand
// out the same string more than once - a button carries shadow and highlight copies of its label,
// and a heading is often repeated by a smaller pane beside it - and now that every label is read
// rather than a chosen few, the shorter one turning up first is routine.
std::string Join(const std::vector<std::string>& parts) {
    std::string result;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (parts[i].empty()) {
            continue;
        }
        bool covered = false;
        for (std::size_t j = 0; j < parts.size() && !covered; ++j) {
            if (i == j || parts[j].find(parts[i]) == std::string::npos) {
                continue;
            }
            // Identical parts cover each other; keeping the first is what makes that terminate.
            covered = parts[j].size() > parts[i].size() || j < i;
        }
        if (covered) {
            continue;
        }
        if (!result.empty()) {
            result += ". ";
        }
        result += parts[i];
    }
    return result;
}

// Arriving on a screen.
//
// A screen with a cursor announces the item under it, and nothing else. Reciting the whole page is
// what the spec forbids by name, and reading every label was doing exactly that - the main menu
// listed all four of its buttons before saying which one was selected. The full label set is still
// snapshot, because the next cursor move needs something to diff against.
//
// A screen with nothing selectable at all - the strap warning, the title screen's "press A" - is the
// one case that does read everything, because there is no navigation to follow and silence would
// look like a freeze.
//
// The test is whether the page has selectable controls, NOT whether one is focused right now. On the
// frame a page settles the manipulator often has not picked its initial control yet, and reading
// "no focus" as "nothing to select" made every return to the main menu recite its four buttons.
// Saying nothing here is safe: focus resolves a frame later and the cursor path announces it.
void AnnounceScreen(std::uint32_t page, std::uint32_t focused, const std::string& focusedText) {
    g_announcedPage = page;
    g_lastFocused = focused;
    g_spokenFocusText = focusedText;
    g_labelText.clear();

    const PageLabels labels = ReadLabels(page, focused);
    std::vector<std::string> screenParts;
    std::vector<std::string> itemParts;
    for (const std::uint32_t label : labels.controls) {
        const std::string text = ReadControlText(label);
        g_labelText[label] = text;  // seeded even when empty, so the first change reads as one
        if (text.empty()) {
            continue;
        }
        (IsItemBound(label) ? itemParts : screenParts).push_back(text);
    }

    std::vector<std::string> parts;
    if (!labels.anySelectable) {
        parts = std::move(screenParts);
    } else {
        parts.push_back(focusedText);
        parts.insert(parts.end(), itemParts.begin(), itemParts.end());
    }

    const std::string announcement = Join(parts);
    if (announcement.empty()) {
        RT_LOGF(RT_TAG_A11Y, "page %08x settled with nothing readable\n", page);
        return;
    }
    ScreenReader::Instance().Speak(announcement, /*interrupt=*/true);
}

// Moving the cursor: the item, plus whichever labels moved with it. A button that names itself is
// the common case; when it does not - character and kart selection - the screen keeps the name of
// the highlighted item in a label of its own, and that label is what just changed.
void AnnounceItem(std::uint32_t page, std::uint32_t focused, const std::string& focusedText) {
    g_lastFocused = focused;
    g_spokenFocusText = focusedText;

    std::vector<std::string> parts{focusedText};
    for (const std::uint32_t label : ReadLabels(page, focused).controls) {
        const std::string text = ReadControlText(label);
        const auto known = g_labelText.find(label);
        const bool changed = known == g_labelText.end() ? !text.empty() : known->second != text;
        g_labelText[label] = text;
        if (!changed || text.empty()) {
            continue;  // a label going blank is a screen tidying up, not something to say
        }
        LearnItemBound(label);
        parts.push_back(text);
    }

    const std::string announcement = Join(parts);
    if (!announcement.empty()) {
        ScreenReader::Instance().Speak(announcement, /*interrupt=*/true);
    }
}

// The cursor has not moved, so anything that changed is a value the player just edited - left and
// right on an up/down control, which repaints a label beside it rather than the control itself.
//
// Only the new value is spoken: the spec draws the line between entering an item, which gets the
// full context, and changing one, which gets the change alone. Repeating the label - "Difficulty:
// Hard" - is called out as wrong by name.
//
// It does interrupt, which is a deliberate departure from the spec's "incremental information does
// not interrupt". Scrubbing through values is faster than speech, so queueing left the reader a
// value or two behind the cursor - the player heard the option they had already passed. See
// docs/menu-accessibility.md section 2.
//
// Nothing is learned from this. A label that changes while the cursor sits still belongs to the
// control the cursor is on, not to whichever item is focused, so treating it as item-bound would
// misorder every later arrival announcement.
void AnnounceValueChange(std::uint32_t page, std::uint32_t focused) {
    const PageLabels labels = ReadLabels(page, focused);

    // A page the cursor cannot reach at all was already read once on arrival, and nothing on it is
    // a value the player is scrubbing through. The race HUD is exactly that page, and re-diffing it
    // every frame turned the lap counter, position and timer into speech that interrupted itself
    // several times a second. It also skips the per-frame pane walk over every HUD element, which
    // with nothing selectable is the most expensive case there is.
    if (!labels.anySelectable) {
        return;
    }

    std::vector<std::string> parts;
    for (const std::uint32_t label : labels.controls) {
        const std::string text = ReadControlText(label);
        const auto known = g_labelText.find(label);
        const bool changed = known == g_labelText.end() ? !text.empty() : known->second != text;
        g_labelText[label] = text;
        if (changed && !text.empty()) {
            parts.push_back(text);
        }
    }

    const std::string announcement = Join(parts);
    if (!announcement.empty()) {
        ScreenReader::Instance().Speak(announcement, /*interrupt=*/true);
    }
}

}  // namespace

void ResetScreenWatcher() {
    g_announcedPage = 0;
    g_lastFocused = 0;
    g_spokenFocusText.clear();
    g_labelText.clear();
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

    const std::uint32_t focused = FocusedControl(page);
    const std::string focusedText = focused != 0 ? ReadControlText(focused) : std::string{};

    if (page != g_announcedPage) {
        AnnounceScreen(page, focused, focusedText);
        return;
    }
    if (focused != g_lastFocused || focusedText != g_spokenFocusText) {
        AnnounceItem(page, focused, focusedText);
        return;
    }
    AnnounceValueChange(page, focused);
}

}  // namespace a11y::ui
