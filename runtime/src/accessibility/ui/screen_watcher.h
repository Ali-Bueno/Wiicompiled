#ifndef MKW_ACCESSIBILITY_UI_SCREEN_WATCHER_H
#define MKW_ACCESSIBILITY_UI_SCREEN_WATCHER_H

namespace a11y::ui {

// Watches what the game is showing and speaks the difference, once per frame.
//
// Nothing here is event-driven, on purpose. Reacting to Page::Activate meant guessing when a
// page's initial focus lands and when its entrance animation ends, and every guess that fixed one
// screen broke another; and listening for PushButton selection events only ever covered plain
// buttons, leaving character, kart and Mii selection silent. The game already answers both: the
// section knows which page is on top and when it has settled, and the page's manipulator knows
// which control has the cursor, whatever kind of control it is.
//
// Entering a screen speaks a dialog's message, if it has one, then the focused item. Moving the
// cursor speaks the focused item. A menu's title is deliberately not spoken.
void TickScreenWatcher();

}  // namespace a11y::ui

#endif  // MKW_ACCESSIBILITY_UI_SCREEN_WATCHER_H
