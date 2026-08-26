#ifndef MKW_ACCESSIBILITY_UI_LYT_WALK_H
#define MKW_ACCESSIBILITY_UI_LYT_WALK_H

#include <cstdint>
#include <string>

namespace a11y::ui {

// Teaches the walker what an nw4r::lyt::TextBox looks like, from a live instance seen in the
// TextBox::SetString hook. Doing it this way means no hardcoded vtable or RTTI address: the value
// is taken from the running game, which is what PRINCIPLES.md section 4 asks for.
void NoteTextBoxInstance(std::uint32_t textBox);

// Returns the text a menu control is currently showing, by walking its layout's pane tree and
// reading the string buffer of every TextBox under it.
//
// This reads the final state rather than intercepting whatever produced it, so it works for text
// baked into the .brlyt, text pushed from BMG, and text composed by Text::PaneHandler alike.
// Returns an empty string if the control has no layout yet or nothing readable. Never throws.
std::string ReadControlText(std::uint32_t control) noexcept;

}  // namespace a11y::ui

#endif  // MKW_ACCESSIBILITY_UI_LYT_WALK_H
