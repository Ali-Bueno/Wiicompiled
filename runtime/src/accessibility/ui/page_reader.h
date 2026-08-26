#ifndef MKW_ACCESSIBILITY_UI_PAGE_READER_H
#define MKW_ACCESSIBILITY_UI_PAGE_READER_H

#include <cstdint>
#include <string>

namespace a11y::ui {

// Screens with nothing to select - the strap warning, the title screen's "press A", the battery
// notice, a message box - never fire a selection event, so the focus path leaves them silent. They
// are the first thing a blind player meets, and silence there reads as a frozen game.
//
// Page::Activate is the one hook every page passes through, but it runs before the entrance
// animation, so the read is deferred a couple of frames.

// From the Page::Activate hook.
void QueuePageForReading(std::uint32_t page);

// From the per-frame tick; announces a queued page once it has settled.
void TickPageReader();

// Everything the page is showing, in on-screen order, or empty.
std::string ReadPageText(std::uint32_t page, bool requireOpaque) noexcept;

// Whether the page routes input to selectable controls. A page without one has no focusable
// widget, which is exactly the case the focus path cannot serve.
bool PageHasFocusableControls(std::uint32_t page) noexcept;

}  // namespace a11y::ui

#endif  // MKW_ACCESSIBILITY_UI_PAGE_READER_H
