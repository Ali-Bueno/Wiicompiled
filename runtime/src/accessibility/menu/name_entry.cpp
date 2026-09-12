#include "accessibility/menu/name_entry.h"

#include "accessibility/guest_text.h"
#include "accessibility/localization.h"
#include "accessibility/screen_reader.h"
#include "runtime_config.h"

namespace a11y::menu {
namespace {

// A single character read back, or its name when it would be heard as nothing.
std::string Spell(const std::u16string& unit) {
    if (unit == u" ") {
        return loc::Get("char_space");
    }
    return Utf16ToUtf8(unit);
}

void Say(const std::string& text) {
    ScreenReader::Instance().Speak(text, /*interrupt=*/true);
}

}  // namespace

void NameEntry::Begin(const std::string& current) {
    mDraft.clear();
    Say(loc::Format("name_edit_begin",
                    {{"n", std::to_string(RuntimeConfigFile::kMiiNameMaxChars)},
                     {"name", current.empty() ? loc::Get("name_unset") : current}}));
}

void NameEntry::Type(const std::string& utf8) {
    const std::u16string units = Utf8ToUtf16(utf8);
    if (units.empty()) {
        return;
    }
    // The record counts UTF-16 units, so the limit is applied in units too.
    if (mDraft.size() + units.size() > RuntimeConfigFile::kMiiNameMaxChars) {
        Say(loc::Get("name_edit_full"));
        return;
    }
    mDraft += units;
    Say(Spell(units));
}

void NameEntry::Backspace() {
    if (mDraft.empty()) {
        Say(loc::Get("name_edit_empty"));
        return;
    }
    const std::u16string last = mDraft.substr(mDraft.size() - 1);
    mDraft.pop_back();
    Say(loc::Format("name_deleted", {{"c", Spell(last)}}));
}

std::string NameEntry::Draft() const {
    return Utf16ToUtf8(mDraft);
}

}  // namespace a11y::menu
