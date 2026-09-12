#ifndef MKW_ACCESSIBILITY_MENU_NAME_ENTRY_H
#define MKW_ACCESSIBILITY_MENU_NAME_ENTRY_H

#include <string>

namespace a11y::menu {

// Typing the online name inside the settings menu: the draft of one row, spoken as it is typed.
// Characters arrive as UTF-8 already resolved against the keyboard layout (menu_input.cpp).
class NameEntry {
public:
    // Speaks the instructions and the current name.
    void Begin(const std::string& current);
    void Type(const std::string& utf8);
    void Backspace();
    // The draft as UTF-8; empty when nothing was typed.
    std::string Draft() const;

private:
    std::u16string mDraft;
};

}  // namespace a11y::menu

#endif  // MKW_ACCESSIBILITY_MENU_NAME_ENTRY_H
