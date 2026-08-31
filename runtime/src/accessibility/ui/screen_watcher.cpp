#include "screen_watcher.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "accessibility/a11y_log.h"
#include "accessibility/screen_reader.h"
#include "entity_info.h"
#include "focus.h"
#include "layers.h"
#include "lyt_walk.h"
#include "memory.h"
#include "page_controls.h"

namespace a11y::ui {
namespace {

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

// A description belongs to the item it describes, so it must never be the whole sentence. On a grid
// the button is a picture and the label naming it is repainted a frame or two after the cursor
// lands, so the reader holds the announcement back rather than say "medium weight" and name the kart
// afterwards. Bounded, so a screen that genuinely names nothing is not waited on forever - there the
// description is spoken alone, which still beats silence.
constexpr int kMaxNameWaits = 60;
std::uint32_t g_waitingOn = 0;
int g_waits = 0;

bool WaitingForName(std::uint32_t token) {
    if (token != g_waitingOn) {
        g_waitingOn = token;
        g_waits = 0;
    }
    return ++g_waits <= kMaxNameWaits;
}

void DoneWaiting() {
    g_waitingOn = 0;
    g_waits = 0;
}

// The screen's text, by exclusion: every control the cursor cannot reach. That is the whole
// classification, and it needs to know nothing about control classes - which is the point. A text
// class nobody has seen before is a label because the cursor cannot reach it, not because it was on
// a list, so it reads instead of going mute.
struct PageLabels {
    std::vector<std::uint32_t> controls;    // on the page the cursor is on
    std::vector<std::uint32_t> background;  // on the other layers stacked with it
    bool trustworthy = false;
    bool anySelectable = false;
};

PageLabels ReadLabels(const std::vector<std::uint32_t>& layers, std::uint32_t page,
                      std::uint32_t focused) noexcept {
    PageLabels labels;
    labels.anySelectable = !SelectableControls(page).empty();

    // A page with a focused control necessarily has selectable ones. If none came back, the
    // manipulator list did not read, and calling every button a label would recite whole menus -
    // the one thing the spec forbids outright. Announce only the focused item instead.
    //
    // Asked of the cursor's own page, never of the stack: a lower layer's buttons say nothing about
    // whether this page read, and letting them answer would re-enable the per-frame value diff on a
    // page with nothing to select - the race HUD, which then narrates itself over the timer.
    labels.trustworthy = labels.anySelectable || focused == 0;
    if (!labels.trustworthy) {
        return labels;
    }

    // Reachability spans the whole stack, so a button a layer down is never mistaken for text.
    std::unordered_set<std::uint32_t> reachable;
    for (const std::uint32_t layer : layers) {
        const std::vector<std::uint32_t> selectable = SelectableControls(layer);
        reachable.insert(selectable.begin(), selectable.end());
    }
    if (focused != 0) {
        reachable.insert(focused);  // it resolved, so it is reachable whatever the list said
    }
    for (const std::uint32_t layer : layers) {
        std::vector<std::uint32_t>& into = layer == page ? labels.controls : labels.background;
        for (const std::uint32_t control : PageControls(layer)) {
            if (reachable.count(control) == 0) {
                into.push_back(control);
            }
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

// A label whose text moved since the last cursor move, spoken; one that went blank, skipped - that
// is a screen tidying up, not something to say. Every label the stack shows goes through here.
void CollectChangedLabels(const std::vector<std::uint32_t>& labels, bool learn,
                          std::vector<std::string>& parts) {
    for (const std::uint32_t label : labels) {
        const std::string text = ReadControlText(label);
        const auto known = g_labelText.find(label);
        const bool changed = known == g_labelText.end() ? !text.empty() : known->second != text;
        g_labelText[label] = text;
        if (!changed || text.empty()) {
            continue;
        }
        if (learn) {
            LearnItemBound(label);
        }
        parts.push_back(text);
    }
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
void AnnounceScreen(const std::vector<std::uint32_t>& layers, std::uint32_t page,
                    std::uint32_t focused, const std::string& focusedText) {
    g_labelText.clear();

    const PageLabels labels = ReadLabels(layers, page, focused);
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
    // A lower layer is as often the menu underneath a dialog as it is the frame around this page,
    // so it is snapshot but not recited: only a class already seen following the cursor is read,
    // which is what a tooltip is. A screen whose tooltip lives a layer down is therefore quiet the
    // first time and reads from the first cursor move onwards.
    for (const std::uint32_t label : labels.background) {
        const std::string text = ReadControlText(label);
        g_labelText[label] = text;
        if (!text.empty() && IsItemBound(label)) {
            itemParts.push_back(text);
        }
    }

    std::vector<std::string> parts;
    if (!labels.anySelectable) {
        parts = std::move(screenParts);
    } else {
        if (focusedText.empty() && itemParts.empty() && WaitingForName(page)) {
            return;  // the page stays uncommitted, so the next frame comes back here
        }
        parts.push_back(focusedText);
        parts.insert(parts.end(), itemParts.begin(), itemParts.end());
        parts.push_back(DescribeFocusedEntity(layers, focused));
    }

    DoneWaiting();
    g_announcedPage = page;
    g_lastFocused = focused;
    g_spokenFocusText = focusedText;

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
void AnnounceItem(const std::vector<std::uint32_t>& layers, std::uint32_t page,
                  std::uint32_t focused, const std::string& focusedText) {
    const PageLabels labels = ReadLabels(layers, page, focused);
    std::vector<std::string> parts{focusedText};
    CollectChangedLabels(labels.controls, /*learn=*/true, parts);
    CollectChangedLabels(labels.background, /*learn=*/true, parts);

    // Nothing but the focused text and whichever labels moved with it can name the item, so if none
    // of them said anything the name has not been painted yet - wait for it instead of describing an
    // item the player has not been told the name of.
    if (parts.size() == 1 && focusedText.empty() && WaitingForName(focused)) {
        return;  // g_lastFocused is left alone, so the next frame comes back here
    }
    DoneWaiting();
    g_lastFocused = focused;
    g_spokenFocusText = focusedText;
    // Last, because the name of a driver or a kart is painted outside its button, so it arrives as
    // one of the labels above - describing the entity before it had named it read backwards.
    parts.push_back(DescribeFocusedEntity(layers, focused));

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
void AnnounceValueChange(const std::vector<std::uint32_t>& layers, std::uint32_t page,
                         std::uint32_t focused) {
    const PageLabels labels = ReadLabels(layers, page, focused);

    // A page the cursor cannot reach at all was already read once on arrival, and nothing on it is
    // a value the player is scrubbing through. The race HUD is exactly that page, and re-diffing it
    // every frame turned the lap counter, position and timer into speech that interrupted itself
    // several times a second. It also skips the per-frame pane walk over every HUD element, which
    // with nothing selectable is the most expensive case there is.
    if (!labels.anySelectable) {
        return;
    }

    std::vector<std::string> parts;
    CollectChangedLabels(labels.controls, /*learn=*/false, parts);
    CollectChangedLabels(labels.background, /*learn=*/false, parts);

    const std::string announcement = Join(parts);
    if (!announcement.empty()) {
        ScreenReader::Instance().Speak(announcement, /*interrupt=*/true);
    }
}

}  // namespace

void ResetScreenWatcher() {
    DoneWaiting();
    g_announcedPage = 0;
    g_lastFocused = 0;
    g_spokenFocusText.clear();
    g_labelText.clear();
}

void TickScreenWatcher() {
    const std::vector<std::uint32_t> layers = ActiveLayerPages();
    if (layers.empty()) {
        return;  // no section yet, or the top page is still animating in or out
    }
    const std::uint32_t page = layers.back();

    const std::uint32_t focused = FocusedControl(page);
    const std::string focusedText = focused != 0 ? ReadControlText(focused) : std::string{};

    if (page != g_announcedPage) {
        AnnounceScreen(layers, page, focused, focusedText);
        return;
    }
    if (focused != g_lastFocused || focusedText != g_spokenFocusText) {
        AnnounceItem(layers, page, focused, focusedText);
        return;
    }
    AnnounceValueChange(layers, page, focused);
}

}  // namespace a11y::ui
