#include "ui_events.h"

#include <chrono>
#include <string>

#include "accessibility/a11y_log.h"
#include "accessibility/screen_reader.h"
#include "lyt_walk.h"
#include "page_reader.h"
#include "text_capture.h"

namespace a11y::ui {
namespace {

// The playbook's announcer contract specifies a dedup window on an identical message. The Wii
// Remote pointer also fires select/deselect in bursts as it sweeps across buttons, and this is what
// keeps that from stuttering.
constexpr auto kDedupWindow = std::chrono::milliseconds(200);

using Clock = std::chrono::steady_clock;

std::string g_lastSpoken;
Clock::time_point g_lastSpokenAt{};

// The page currently readable. Zero until a page finishes its entrance animation.
std::uint32_t g_currentPage = 0;

void Announce(const std::string& text, bool interrupt) {
    if (text.empty()) {
        return;
    }
    const auto now = Clock::now();
    if (text == g_lastSpoken && (now - g_lastSpokenAt) < kDedupWindow) {
        return;
    }
    g_lastSpoken = text;
    g_lastSpokenAt = now;
    ScreenReader::Instance().Speak(text, interrupt);
}

}  // namespace

void OnControlSelected(std::uint32_t control, bool initial) {
    // Read what the control is showing right now rather than trusting anything we saw it being
    // told earlier: the localized string is written by a path that does not pass through
    // TextBox::SetString, so the events lag the screen by a whole localisation step.
    const std::string text = ReadControlText(control);
    if (text.empty()) {
        RT_LOGF(RT_TAG_A11Y, "selected control %08x: no readable text\n", control);
        return;
    }
    // Both cases interrupt: an initial selection is part of a screen change, and moving the cursor
    // makes the previous item irrelevant.
    Announce(text, /*interrupt=*/true);
    (void)initial;
}

void OnPageEntered(std::uint32_t page) {
    g_currentPage = page;
    RT_LOGF(RT_TAG_A11Y, "page %08x ready\n", page);
}

void OnPageActivated(std::uint32_t page) {
    // Activate fires before the entrance animation, so nothing is readable yet. Clearing here is
    // what makes a re-entered page announce again instead of being diff-gated into silence.
    if (page != g_currentPage) {
        g_lastSpoken.clear();
    }
    QueuePageForReading(page);
}

void OnSectionEntered(std::uint32_t section) {
    ClearCapturedText();
    g_lastSpoken.clear();
    g_currentPage = 0;
    RT_LOGF(RT_TAG_A11Y, "section %08x entered\n", section);
}

}  // namespace a11y::ui
