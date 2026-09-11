#include "accessibility/update_version.h"

#include <algorithm>

namespace a11y::update {
namespace {

// A digit run longer than this is not a version number; clamping keeps a malformed tag from
// overflowing the int it is parsed into.
constexpr int kMaxVersionPart = 999999;

bool IsDigit(char c) {
    return c >= '0' && c <= '9';
}

bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

std::string_view Trim(std::string_view text) {
    while (!text.empty() && IsSpace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && IsSpace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

}  // namespace

std::optional<Version> ParseVersion(std::string_view text) {
    text = Trim(text);
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) {
        text.remove_prefix(1);
    }
    if (text.empty() || !IsDigit(text.front())) {
        return std::nullopt;
    }
    Version version{0, 0, 0};
    size_t pos = 0;
    for (size_t part = 0; part < version.size(); ++part) {
        int value = 0;
        while (pos < text.size() && IsDigit(text[pos])) {
            value = value < kMaxVersionPart ? value * 10 + (text[pos] - '0') : kMaxVersionPart;
            ++pos;
        }
        version[part] = value;
        if (pos >= text.size() || text[pos] != '.') {
            break;  // a suffix such as "-beta" simply ends the number
        }
        ++pos;
    }
    return version;
}

bool IsNewer(const Version& candidate, const Version& installed) {
    return candidate > installed;
}

std::optional<std::string> FindJsonString(std::string_view json, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const size_t keyPos = json.find(needle);
    if (keyPos == std::string_view::npos) {
        return std::nullopt;
    }
    size_t pos = keyPos + needle.size();
    while (pos < json.size() && IsSpace(json[pos])) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != ':') {
        return std::nullopt;
    }
    ++pos;
    while (pos < json.size() && IsSpace(json[pos])) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '"') {
        return std::nullopt;  // null, a number or an object: not a value this scan reads
    }
    ++pos;
    std::string value;
    while (pos < json.size()) {
        const char c = json[pos];
        if (c == '\\') {
            // A tag is plain ASCII; an escape is kept literally rather than decoded.
            if (++pos >= json.size()) {
                return std::nullopt;
            }
            value.push_back(json[pos++]);
            continue;
        }
        if (c == '"') {
            return value;
        }
        value.push_back(c);
        ++pos;
    }
    return std::nullopt;  // unterminated string
}

std::optional<std::string> LastLineFirstToken(std::string_view text) {
    std::string_view lastLine;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t end = std::min(text.find('\n', pos), text.size());
        const std::string_view line = Trim(text.substr(pos, end - pos));
        if (!line.empty()) {
            lastLine = line;
        }
        pos = end + 1;
    }
    if (lastLine.empty()) {
        return std::nullopt;
    }
    size_t tokenEnd = 0;
    while (tokenEnd < lastLine.size() && !IsSpace(lastLine[tokenEnd])) {
        ++tokenEnd;
    }
    return std::string(lastLine.substr(0, tokenEnd));
}

}  // namespace a11y::update
