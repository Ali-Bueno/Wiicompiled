#ifndef MKW_ACCESSIBILITY_LOCALIZATION_H
#define MKW_ACCESSIBILITY_LOCALIZATION_H

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace a11y::loc {

// Loads the phrase table for the Wii system language the game itself runs in
// ([system].language in Config.toml — the value our SCGetLanguage stub serves the game).
// Files live in accessibility_lang\<code>.ini next to the exe; a missing file or key
// falls back to the built-in English defaults, so speech can never go mute.
void Init();

// Text for a key: the language file's value, or the built-in English default.
std::string Get(const std::string& key);

// Get() plus {name} placeholder substitution, e.g. Format("lap", {{"n", "2"}, {"total", "3"}}).
std::string Format(const std::string& key,
                   std::initializer_list<std::pair<std::string_view, std::string>> args);

// Spoken phrase for a 1-based race position. Key choice follows the English ordinal rule
// (1st/2nd/3rd/nth, teens excepted); languages without that rule give all four keys the
// same template, so the rule is inert there.
std::string Position(int value);

}  // namespace a11y::loc

#endif  // MKW_ACCESSIBILITY_LOCALIZATION_H
