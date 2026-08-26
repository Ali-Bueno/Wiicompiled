#include "ui_events.h"

#include "accessibility/a11y_log.h"
#include "text_capture.h"

namespace a11y::ui {

// The selection hooks are left registered but inert: focus is read from the page's manipulator each
// frame instead, which covers every control type rather than plain buttons alone. Removing the
// registrations would force a full retranslation for no gain.
void OnControlSelected(std::uint32_t control, bool initial) {
    (void)control;
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
