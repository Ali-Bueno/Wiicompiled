#include "guest_text.h"

#include <algorithm>
#include <array>
#include <string_view>

#include "memory.h"

namespace a11y {
namespace {

// Check mapping first so a truncated guest buffer does not trigger an access-fault dump.
bool TryRead16(std::uint32_t addr, std::uint16_t& value) noexcept {
    if (!Memory::Contains(addr, sizeof(value))) {
        return false;
    }
    try {
        value = Memory::Read16(addr);
        return true;
    } catch (const Memory::AccessViolation&) {
        return false;
    }
}

// Code points Mario Kart Wii substitutes into composed BMG strings that carry no speakable text:
// button-icon glyphs and similar live in the private use area, and the tag machinery leaves control
// characters behind. Whitespace is kept but normalised by the caller.
bool IsUnspeakable(char32_t cp) {
    const bool isControl = cp < 0x20 || (cp >= 0x7F && cp <= 0x9F);
    const bool isPrivateUse = cp >= 0xE000 && cp <= 0xF8FF;
    const bool isSpecial = cp >= 0xFFF0 && cp <= 0xFFFF;
    return isControl || isPrivateUse || isSpecial;
}

void AppendUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Collapses runs of whitespace and trims the ends. Dropped tag glyphs otherwise leave double spaces
// that some voices pause on.
std::string Tidy(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool pendingSpace = false;
    for (const char c : text) {
        const bool isSpace = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
        if (isSpace) {
            pendingSpace = !out.empty();
            continue;
        }
        if (pendingSpace) {
            out.push_back(' ');
            pendingSpace = false;
        }
        out.push_back(c);
    }
    return out;
}

std::string ConvertUtf16(std::u16string_view text) {
    std::string utf8;
    utf8.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        char32_t cp = text[i];
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (i + 1 == text.size()) {
                break;
            }
            const char16_t low = text[i + 1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((static_cast<char32_t>(cp - 0xD800) << 10) | (low - 0xDC00));
                ++i;
            } else {
                continue;
            }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            continue;
        }
        if (IsUnspeakable(cp)) {
            utf8.push_back(' ');
            continue;
        }
        AppendUtf8(utf8, cp);
    }
    return Tidy(utf8);
}

}  // namespace

std::string Utf16ToUtf8(const std::u16string& text) noexcept try {
    return ConvertUtf16(text);
} catch (...) {
    return {};
}

std::string ReadGuestText(std::uint32_t guestAddr, std::size_t maxChars) noexcept try {
    if (guestAddr == 0) {
        return {};
    }

    const std::size_t limit = std::min(maxChars, kMaxGuestTextChars);
    std::array<char16_t, kMaxGuestTextChars> text;
    std::size_t length = 0;

    for (std::size_t i = 0; i < limit; ++i) {
        std::uint16_t unit = 0;
        if (!TryRead16(guestAddr + static_cast<std::uint32_t>(i * sizeof(std::uint16_t)), unit)) {
            break;
        }
        if (unit == 0) {
            break;
        }

        text[length++] = static_cast<char16_t>(unit);
    }

    return ConvertUtf16({text.data(), length});
} catch (...) {
    return {};
}

std::u16string Utf8ToUtf16(const std::string& text) noexcept try {
    std::u16string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::size_t extra = 0;
        char32_t cp = 0;
        char32_t minimum = 0;
        if (lead < 0x80) {
            cp = lead;
        } else if (lead >= 0xC2 && lead <= 0xDF) {
            cp = lead & 0x1F;
            extra = 1;
            minimum = 0x80;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            cp = lead & 0x0F;
            extra = 2;
            minimum = 0x800;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            cp = lead & 0x07;
            extra = 3;
            minimum = 0x10000;
        } else {
            ++i;  // Invalid lead or stray continuation byte.
            continue;
        }
        bool wellFormed = true;
        for (std::size_t k = 1; k <= extra; ++k) {
            if (k >= text.size() - i) {
                wellFormed = false;
                break;
            }
            const auto c = static_cast<unsigned char>(text[i + k]);
            if ((c & 0xC0) != 0x80) {
                wellFormed = false;
                break;
            }
            cp = (cp << 6) | (c & 0x3F);
        }
        i += wellFormed ? extra + 1 : 1;
        if (!wellFormed || cp < minimum || IsUnspeakable(cp) || cp > 0xFFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF)) {
            continue;
        }
        out.push_back(static_cast<char16_t>(cp));
    }
    return out;
} catch (...) {
    return {};
}

}  // namespace a11y
