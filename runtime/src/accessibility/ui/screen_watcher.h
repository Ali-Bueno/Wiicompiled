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
// What gets read is decided the same way: a control the cursor can reach is an item, and everything
// else on the page is text the screen is showing. No list of known classes, so a screen built out of
// widgets nobody has seen before still reads.
//
// Entering a screen speaks its text, then the focused item, then whatever describes that item.
// Moving the cursor speaks the item and whichever labels moved with it.
void TickScreenWatcher();

// Drops the per-page state. Called when the section changes, because pages and controls are rebuilt
// from scratch and the guest reuses their addresses - a snapshot kept across that would silence a
// new screen that happened to land where the old one was.
void ResetScreenWatcher();

}  // namespace a11y::ui

#endif  // MKW_ACCESSIBILITY_UI_SCREEN_WATCHER_H
