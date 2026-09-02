#include "accessibility/localization.h"

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <unordered_map>

#include "accessibility/a11y_log.h"
#include "runtime_config.h"

namespace a11y::loc {
namespace {

struct Phrase {
    const char* key;
    const char* english;
};

// Built-in English defaults. This table is also the key catalogue: a file line whose key is
// not listed here is reported as unknown instead of silently kept.
constexpr Phrase kEnglishDefaults[] = {
    {"ready", "Mario Kart Wii accessibility ready."},
    {"settings_reloaded", "settings reloaded"},

    {"curve_left", "left"},
    {"curve_right", "right"},
    {"curve_left_long", "left long"},
    {"curve_right_long", "right long"},
    {"curve_gentle_left", "gentle left"},
    {"curve_gentle_right", "gentle right"},
    {"curve_gentle_left_long", "gentle left long"},
    {"curve_gentle_right_long", "gentle right long"},
    {"curve_hard_left", "hard left"},
    {"curve_hard_right", "hard right"},
    {"curve_hard_left_long", "hard left long"},
    {"curve_hard_right_long", "hard right long"},
    {"curve_hairpin_left", "hairpin left"},
    {"curve_hairpin_right", "hairpin right"},
    {"curve_hairpin_left_long", "hairpin left long"},
    {"curve_hairpin_right_long", "hairpin right long"},
    {"curve_then", " then "},

    {"position_1", "{n}st"},
    {"position_2", "{n}nd"},
    {"position_3", "{n}rd"},
    {"position_other", "{n}th"},

    {"lap", "lap {n} of {total}"},
    {"finish", "finished"},
    {"finish_position", "finished {pos}"},

    {"off_road", "off road"},
    {"off_road_fall", "off road, edge drop"},
    {"on_road", "on road"},
    {"wrong_way", "wrong way"},

    {"menu_opened",
     "accessibility settings. Up and down to choose, left and right to adjust, A to activate, B to close."},
    {"menu_closed", "settings closed"},
    {"menu_race_blocked", "settings open from the menus, not during a race"},
    {"opt_master_volume", "master volume"},
    {"opt_music_volume", "music volume"},
    {"opt_kart_volume", "my kart volume"},
    {"opt_rival_volume", "rival karts volume"},
    {"opt_roulette_volume", "item roulette volume"},
    {"opt_steering_strength", "steering guide strength"},
    {"opt_look_ahead", "anticipation"},
    {"opt_invert_pan", "invert steering pan"},
    {"opt_edge_cues", "edge cues"},
    {"demo_edge", "hear the edge tone"},
    {"demo_curve", "hear a curve beep"},
    {"demo_itembox", "hear the item box"},

    // Spoken after the row's name and value. One "<key>_help" per option, by the naming
    // convention settings_menu.cpp uses; a row without one simply reads name and value.
    {"opt_master_volume_help",
     "Volume of the whole game: engine, music and effects together. Screen reader speech is not "
     "affected."},
    {"opt_music_volume_help", "Background music only. Engines and effects keep their own volume."},
    {"opt_kart_volume_help",
     "Your own kart: engine, drift and your driver's voice. Above one hundred it is louder than "
     "the original game, which helps you hear the steering guide that rides on it."},
    {"opt_rival_volume_help",
     "The other karts' engines and voices. Lower it to stop the pack covering your own engine."},
    {"opt_roulette_volume_help",
     "The item roulette spin and the jingle when your item is decided. Menu clicks are left "
     "alone."},
    {"opt_steering_strength_help",
     "How hard the engine pans when the kart points away from the racing line. This is the main "
     "part of the guide: raise it for a wider, more obvious swing, lower it for a subtler one."},
    {"opt_look_ahead_help",
     "How far ahead in time the mod looks, from a tenth of a second up to about eight "
     "tenths. Raise it to be warned of corners and of the road edge earlier, lower it to "
     "follow what is happening right now."},
    {"opt_invert_pan_help",
     "Off, the engine sounds toward the side you must steer away from. On flips it, so you steer "
     "toward the sound."},
    {"opt_edge_cues_help",
     "Beeps that quicken as you near the edge of the road, and a held tone while you are off it. "
     "Turning it off keeps the spoken off road and wrong way calls."},
    {"demo_edge_help", "Plays the held tone that sounds while the kart is off the road."},
    {"demo_curve_help", "Plays the beep that marks a corner coming up."},
    {"demo_itembox_help",
     "Plays the blip that points at a nearby item box while your hands are empty."},

    {"weight_light", "light weight"},
    {"weight_medium", "medium weight"},
    {"weight_heavy", "heavy weight"},
    {"entity_locked", "locked"},
    {"stats_intro", "stats"},
    {"stat_speed", "speed"},
    {"stat_weight", "weight"},
    {"stat_acceleration", "acceleration"},
    {"stat_handling", "handling"},
    {"stat_drift", "drift"},
    {"stat_offroad", "off road"},
    {"stat_miniturbo", "mini turbo"},
    {"stat_value", "{name} {n}"},

    {"value_on", "on"},
    {"value_off", "off"},
    {"percent", "{n} percent"},
};

// Wii SYSCONF IPL.LNG values, as the port documents them (runtime_config.h SystemLanguage():
// 0 JP, 1 EN, 2 DE, 3 FR, 4 ES, 5 IT, 6 NL, 7 SCh, 8 TCh, 9 KO). The PAL disc carries only
// EN/FR/DE/ES/IT and falls back internally for the rest — the mod falls back the same way.
constexpr int32_t kScEnglish = 1;
constexpr int32_t kScGerman = 2;
constexpr int32_t kScFrench = 3;
constexpr int32_t kScItalian = 5;

const char* LanguageCode(int32_t scLanguage) {
    if (scLanguage == kScEnglish) return "en";
    if (scLanguage == kScGerman) return "de";
    if (scLanguage == kScFrench) return "fr";
    if (scLanguage == RuntimeConfigFile::kSystemLanguageSpanish) return "es";
    if (scLanguage == kScItalian) return "it";
    return "en";
}

std::unordered_map<std::string, std::string> g_phrases;

std::string_view TrimView(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

// Values wanting leading/trailing spaces (the "curve_then" joiner) wrap them in quotes.
std::string_view Unquote(std::string_view value) {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value.remove_prefix(1);
        value.remove_suffix(1);
    }
    return value;
}

}  // namespace

std::filesystem::path ExeDirectory() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(std::wstring(buffer, length)).parent_path();
}

namespace {

void LoadLanguageFile(const std::filesystem::path& path, const char* code) {
    std::ifstream file(path);
    if (!file.is_open()) {
        RT_LOGF(RT_TAG_A11Y, "localization: no file for '%s', using built-in English\n", code);
        return;
    }
    int applied = 0;
    std::string line;
    while (std::getline(file, line)) {
        const std::string_view trimmed = TrimView(line);
        if (trimmed.empty() || trimmed.front() == '#' || trimmed.front() == ';') {
            continue;
        }
        const size_t equals = trimmed.find('=');
        if (equals == std::string_view::npos) {
            continue;
        }
        const std::string key(TrimView(trimmed.substr(0, equals)));
        const std::string_view value = Unquote(TrimView(trimmed.substr(equals + 1)));
        const auto it = g_phrases.find(key);
        if (it == g_phrases.end()) {
            RT_LOGF(RT_TAG_A11Y, "localization: unknown key '%s' in %s.ini\n", key.c_str(), code);
            continue;
        }
        it->second.assign(value);
        ++applied;
    }
    RT_LOGF(RT_TAG_A11Y, "localization: '%s' loaded, %d phrases\n", code, applied);
}

}  // namespace

void Init() {
    g_phrases.clear();
    for (const Phrase& phrase : kEnglishDefaults) {
        g_phrases.emplace(phrase.key, phrase.english);
    }
    const char* code = LanguageCode(RuntimeConfigFile::SystemLanguage());
    LoadLanguageFile(ExeDirectory() / "accessibility_lang" / (std::string(code) + ".ini"), code);
}

std::string Get(const std::string& key) {
    const auto it = g_phrases.find(key);
    if (it == g_phrases.end()) {
        RT_LOGF(RT_TAG_A11Y, "localization: missing key '%s'\n", key.c_str());
        return key;  // audible and debuggable, unlike silence
    }
    return it->second;
}

bool Has(const std::string& key) {
    return g_phrases.find(key) != g_phrases.end();
}

std::string Format(const std::string& key,
                   std::initializer_list<std::pair<std::string_view, std::string>> args) {
    std::string text = Get(key);
    for (const auto& [name, value] : args) {
        const std::string token = "{" + std::string(name) + "}";
        for (size_t pos = text.find(token); pos != std::string::npos;
             pos = text.find(token, pos + value.size())) {
            text.replace(pos, token.size(), value);
        }
    }
    return text;
}

std::string Position(int value) {
    const int lastTwo = value % 100;
    const char* key = "position_other";
    if (lastTwo < 11 || lastTwo > 13) {  // 11th–13th break the last-digit pattern
        switch (value % 10) {
            case 1: key = "position_1"; break;
            case 2: key = "position_2"; break;
            case 3: key = "position_3"; break;
            default: break;
        }
    }
    return Format(key, {{"n", std::to_string(value)}});
}

}  // namespace a11y::loc
