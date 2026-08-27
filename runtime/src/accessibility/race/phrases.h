#ifndef MKW_ACCESSIBILITY_RACE_PHRASES_H
#define MKW_ACCESSIBILITY_RACE_PHRASES_H

#include "runtime_config.h"

namespace a11y::race {

// Race speech follows the Wii system language the player already picked in Config.toml. Only
// Spanish exists beyond English so far - the language of the blind player testing this; a
// play-test transcribed "hairpin" as "airping", which is what a call in the wrong language is
// worth. Menu text needs none of this: it is captured from the game, already localized.
inline bool SpeakSpanish() {
    return RuntimeConfigFile::SystemLanguage() == RuntimeConfigFile::kSystemLanguageSpanish;
}

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_PHRASES_H
