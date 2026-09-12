#ifndef MKW_ACCESSIBILITY_MENU_SETTINGS_MENU_H
#define MKW_ACCESSIBILITY_MENU_SETTINGS_MENU_H

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "accessibility/menu/name_entry.h"

namespace a11y::menu {

// Abstract inputs, so the same menu is driven by L3+R3/dpad on a gamepad and by F8/arrows on the
// keyboard. Events arrive on the host event thread; the menu itself runs on the guest frame tick.
enum class MenuAction {
    Toggle,     // open/close (L3+R3, F8)
    Up,
    Down,
    Left,
    Right,
    Activate,   // A / Enter: toggles a switch, plays a demo, saves a typed name
    Back,       // B / Escape: close, or cancel a typed name
    Backspace,  // while typing a name
    Text,       // while typing a name: one character, UTF-8
};

struct MenuInput {
    MenuAction action;
    std::string text;
};

// The self-voicing settings menu: narrated through the screen reader, never drawn. While open it
// blocks the guest pad (the same PADBlockInput the F10 bar uses), so navigating it cannot steer
// the game's own menu underneath. Race-time opening is refused: the game menus are its place.
class SettingsMenu {
public:
    static SettingsMenu& Instance();

    bool IsOpen() const { return mOpen; }
    // A row is taking typed text: the keyboard spells instead of navigating.
    bool IsEditingText() const { return mEditing.load(std::memory_order_relaxed); }

    // Any thread; consumed by Tick on the guest thread.
    void Enqueue(MenuAction action, std::string text = std::string());

    void Tick();

private:
    SettingsMenu() = default;

    // A row is a value (name + spoken value + adjust) or an action (name + activate), or both
    // for switches, which toggle from Activate as well as from Left/Right.
    struct Option {
        std::string nameKey;
        std::function<std::string()> value;
        std::function<void(int)> adjust;    // -1 left, +1 right
        std::function<void()> activate;
    };

    void BuildOptions();
    void Apply(const MenuInput& input);
    void ApplyEditing(const MenuInput& input);
    void Open();
    void Close();
    void SpeakFocused(bool withName, bool interrupt);
    void BeginNameEdit();
    void EndNameEdit(bool save);

    std::vector<Option> mOptions;
    int mFocus = 0;
    bool mOpen = false;
    bool mBuilt = false;
    std::atomic<bool> mEditing{false};
    NameEntry mNameEntry;

    std::mutex mQueueMutex;
    std::vector<MenuInput> mQueue;
};

}  // namespace a11y::menu

#endif  // MKW_ACCESSIBILITY_MENU_SETTINGS_MENU_H
