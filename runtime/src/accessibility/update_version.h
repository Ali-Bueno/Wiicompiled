#ifndef MKW_ACCESSIBILITY_UPDATE_VERSION_H
#define MKW_ACCESSIBILITY_UPDATE_VERSION_H

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace a11y::update {

// Three integers and nothing else, the rule the installer already applies to its own tags
// (installer/src/update.rs, Version::parse).
using Version = std::array<int, 3>;

// Leading "v" dropped, missing parts zero. std::nullopt when the text does not start with a
// digit, so a missing or corrupt value never reads as 0.0.0 and makes every release look newer.
std::optional<Version> ParseVersion(std::string_view text);

bool IsNewer(const Version& candidate, const Version& installed);

// Minimal scan for one string value of a JSON object. The runtime links no JSON library
// (checked: Dependencies/ and runtime/third_party/ have none) and the release endpoint returns a
// single object, whose first "tag_name" is its own.
std::optional<std::string> FindJsonString(std::string_view json, std::string_view key);

// The published Retro Rewind version list is one release per line, newest last; only the first
// whitespace-separated token of a line is the version.
std::optional<std::string> LastLineFirstToken(std::string_view text);

}  // namespace a11y::update

#endif  // MKW_ACCESSIBILITY_UPDATE_VERSION_H
