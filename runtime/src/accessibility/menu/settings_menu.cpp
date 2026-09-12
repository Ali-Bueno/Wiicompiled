#include "accessibility/menu/settings_menu.h"

#include "accessibility/localization.h"
#include "accessibility/mii/mii_identity.h"
#include "accessibility/race/race_state.h"
#include "accessibility/screen_reader.h"
#include "dolphin/pad.h"
#include "runtime_config.h"

namespace a11y::menu {
namespace {

// A row's description lives under its own name key plus this suffix, so a new option carries its
// explanation by naming convention rather than by a second field nobody remembers to fill.
constexpr const char* kHelpKeySuffix = "_help";

void Say(const std::string& text) {
    // Interrupting keeps the reader on the cursor while scrubbing; queued speech lagged a value
    // behind it in the MK64 menu (docs/menu-accessibility.md §2).
    ScreenReader::Instance().Speak(text, /*interrupt=*/true);
}

}  // namespace

SettingsMenu& SettingsMenu::Instance() {
    static SettingsMenu instance;
    return instance;
}

void SettingsMenu::Enqueue(MenuAction action, std::string text) {
    std::lock_guard<std::mutex> lock(mQueueMutex);
    mQueue.push_back({action, std::move(text)});
}

void SettingsMenu::Tick() {
    std::vector<MenuInput> inputs;
    {
        std::lock_guard<std::mutex> lock(mQueueMutex);
        inputs.swap(mQueue);
    }
    for (const MenuInput& input : inputs) {
        Apply(input);
    }
    // The F10 bar rewrites PADBlockInput from its own state every frame, just before this tick
    // (settings_overlay.cpp Draw), so the block only holds if it is re-asserted here. On close
    // that same write releases it, and swallows the keys still held.
    if (mOpen) {
        PADBlockInput(true);
    }
}

void SettingsMenu::Open() {
    // The menus are its place: mid-race the driving cues own the audio, and the blocked pad
    // would freeze the kart.
    if (race::ReadRaceState().valid) {
        Say(loc::Get("menu_race_blocked"));
        return;
    }
    BuildOptions();
    mOpen = true;
    mFocus = 0;
    Say(loc::Get("menu_opened"));
    // Queued, so the welcome line is heard before the first row.
    SpeakFocused(/*withName=*/true, /*interrupt=*/false);
}

void SettingsMenu::Close() {
    mOpen = false;
    mEditing.store(false, std::memory_order_relaxed);
    Say(loc::Get("menu_closed"));
}

void SettingsMenu::BeginNameEdit() {
    mEditing.store(true, std::memory_order_relaxed);
    mNameEntry.Begin(RuntimeConfigFile::MiiName());
}

void SettingsMenu::EndNameEdit(bool save) {
    mEditing.store(false, std::memory_order_relaxed);
    const std::string name = save ? mNameEntry.Draft() : std::string();
    if (name.empty()) {
        Say(loc::Get("name_unchanged"));
        return;
    }
    if (!RuntimeConfigFile::SetMiiName(name)) {
        Say(loc::Get("name_save_failed"));
        return;
    }
    // The database record and the licence follow on the next mii tick.
    mii::Refresh();
    Say(loc::Format("name_saved", {{"name", name}}));
}

void SettingsMenu::ApplyEditing(const MenuInput& input) {
    switch (input.action) {
        case MenuAction::Toggle:
            Close();
            break;
        case MenuAction::Back:
            EndNameEdit(/*save=*/false);
            break;
        case MenuAction::Activate:
            EndNameEdit(/*save=*/true);
            break;
        case MenuAction::Backspace:
            mNameEntry.Backspace();
            break;
        case MenuAction::Text:
            mNameEntry.Type(input.text);
            break;
        default:
            break;  // arrows spell nothing
    }
}

void SettingsMenu::SpeakFocused(bool withName, bool interrupt) {
    if (mOptions.empty()) {
        return;
    }
    const Option& option = mOptions[static_cast<size_t>(mFocus)];
    std::string text = withName ? loc::Get(option.nameKey) : std::string();
    if (option.value) {
        if (!text.empty()) {
            text += ", ";
        }
        text += option.value();
    }
    // Description last, the way the game-menu narration reads an item and then its tooltip:
    // scrubbing the list interrupts it, dwelling on a row hears it in full.
    if (withName) {
        const std::string helpKey = option.nameKey + kHelpKeySuffix;
        if (loc::Has(helpKey)) {
            if (!text.empty()) {
                text += ". ";
            }
            text += loc::Get(helpKey);
        }
    }
    if (!text.empty()) {
        ScreenReader::Instance().Speak(text, interrupt);
    }
}

void SettingsMenu::Apply(const MenuInput& input) {
    if (!mOpen) {
        if (input.action == MenuAction::Toggle) {
            Open();
        }
        return;
    }
    if (IsEditingText()) {
        ApplyEditing(input);
        return;
    }
    const MenuAction action = input.action;
    switch (action) {
        case MenuAction::Toggle:
        case MenuAction::Back:
            Close();
            break;
        case MenuAction::Up:
        case MenuAction::Down: {
            const int count = static_cast<int>(mOptions.size());
            const int delta = action == MenuAction::Up ? -1 : 1;
            mFocus = (mFocus + delta + count) % count;
            SpeakFocused(/*withName=*/true, /*interrupt=*/true);
            break;
        }
        case MenuAction::Left:
        case MenuAction::Right: {
            Option& option = mOptions[static_cast<size_t>(mFocus)];
            if (option.adjust) {
                option.adjust(action == MenuAction::Left ? -1 : 1);
                SpeakFocused(/*withName=*/false, /*interrupt=*/true);
            }
            break;
        }
        case MenuAction::Activate: {
            Option& option = mOptions[static_cast<size_t>(mFocus)];
            if (option.activate) {
                option.activate();
                // A row that started taking text has just spoken its instructions.
                if (option.value && !IsEditingText()) {
                    SpeakFocused(/*withName=*/false, /*interrupt=*/true);
                }
            }
            break;
        }
        default:
            break;  // Backspace and Text only exist while a row takes text
    }
}

}  // namespace a11y::menu
