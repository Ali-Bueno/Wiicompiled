#ifndef MKW_ACCESSIBILITY_UI_TEXT_CAPTURE_H
#define MKW_ACCESSIBILITY_UI_TEXT_CAPTURE_H

#include <cstdint>
#include <string>

namespace a11y::ui {

// The localized text a menu shows never reaches nw4r's TextBox buffer: that buffer keeps the
// Japanese string baked into the .brlyt, and Mario Kart Wii's own Text:: system composes the
// translated string and draws it glyph by glyph. Proven on screen - the pane named "new" holds
// "NEW" while the game displays "NUEVO".
//
// So the text is captured as it is composed, one code unit at a time, and indexed by the TextBox it
// belongs to. The pane walk then asks here instead of reading the buffer.

// Text::GlobalHandler::SetPaneHandler - starts composing for a new pane. `paneHandler` points at
// its TextBox, which is how a captured string gets attributed.
void BeginPaneText(std::uint32_t paneHandler);

// Text::GlobalHandler::SetCharacter - one composed UTF-16 code unit.
void AppendPaneChar(std::uint16_t codeUnit);

// What this TextBox is currently showing, or empty if nothing was captured for it.
std::string TextForTextBox(std::uint32_t textBox) noexcept;

// Menus are rebuilt constantly and guest pointers get recycled, so the cache is dropped whenever
// the section changes.
void ClearCapturedText();

}  // namespace a11y::ui

#endif  // MKW_ACCESSIBILITY_UI_TEXT_CAPTURE_H
