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
    {"on_road", "on road"},
    {"wrong_way", "wrong way"},
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

std::filesystem::path ExeDirectory() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(std::wstring(buffer, length)).parent_path();
}

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
