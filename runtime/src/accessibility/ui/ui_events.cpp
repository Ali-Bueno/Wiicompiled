#include "ui_events.h"

#include "accessibility/a11y_log.h"
#include "lyt_walk.h"
#include "screen_watcher.h"
#include "text_capture.h"

namespace a11y::ui {

void OnControlSelected(std::uint32_t control, bool initial) {
    // Learned first, and unconditionally. A page gives its initial focus while it is still fading
    // in, so the control has no readable text yet - returning early on that would mean never
    // learning that it is a button, and the screen announcement would then recite every button on
    // the page as if it were a title.
    NotePushButtonInstance(control);

    const std::string text = ReadControlText(control);
    if (text.empty()) {
        return;
    }
    // Recorded, never spoken from here. What gets said and when is decided once per frame by the
    // screen watcher, so a selection that happens while a new screen is still animating in lands in
    // that screen's announcement instead of racing ahead of it.
    NoteFocusedControl(control, text);
    (void)initial;
}

void OnPageEntered(std::uint32_t page) {
    (void)page;
}

void OnPageActivated(std::uint32_t page) {
    (void)page;
}

void OnSectionEntered(std::uint32_t section) {
    // Pages and their panes are rebuilt from scratch, so the captured text is about to describe
    // objects that no longer exist.
    ClearCapturedText();
    RT_LOGF(RT_TAG_A11Y, "section %08x entered\n", section);
}

}  // namespace a11y::ui
