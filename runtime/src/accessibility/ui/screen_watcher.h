#ifndef MKW_ACCESSIBILITY_UI_SCREEN_WATCHER_H
#define MKW_ACCESSIBILITY_UI_SCREEN_WATCHER_H

#include <cstdint>
#include <string>

namespace a11y::ui {

// Watches what the game is showing and speaks the difference, once per frame.
//
// This deliberately replaces an earlier event-driven design. Reacting to Page::Activate meant
// guessing when the page's initial focus lands, when the entrance animation ends, and which of
// several pages activating in one frame is the real one - and every guess that fixed one screen
// broke another. The game already answers all of it: the section knows which page is on top, and
// the page knows when it has settled. Reading that each frame and diffing needs no ordering
// assumptions at all.
//
// On a new screen it speaks the non-button text - the title, a dialog's message - followed by the
// focused item. On the same screen it speaks the focused item whenever it changes.

// From the selection hooks. Records only; the frame tick decides what is spoken and when.
void NoteFocusedControl(std::uint32_t control, std::string text);

// From the per-frame tick.
void TickScreenWatcher();

}  // namespace a11y::ui

#endif  // MKW_ACCESSIBILITY_UI_SCREEN_WATCHER_H
