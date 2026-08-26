#include "guest_text.h"

#include <algorithm>

#include "memory.h"

namespace a11y {
namespace {

// Memory only exposes TryRead32; this mirrors its pattern for the 16-bit reads a UTF-16 string
// needs. Unmapped memory must end the read, never take the game down.
bool TryRead16(std::uint32_t addr, std::uint16_t& value) noexcept {
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

}  // namespace

std::string Utf16ToUtf8(const std::u16string& text) noexcept {
    std::string utf8;
    utf8.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        char32_t cp = text[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < text.size()) {
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

std::string ReadGuestText(std::uint32_t guestAddr, std::size_t maxChars) noexcept {
    if (guestAddr == 0) {
        return {};
    }

    const std::size_t limit = std::min(maxChars, kMaxGuestTextChars);
    std::string utf8;
    utf8.reserve(limit);

    for (std::size_t i = 0; i < limit; ++i) {
        std::uint16_t unit = 0;
        if (!TryRead16(guestAddr + static_cast<std::uint32_t>(i * sizeof(std::uint16_t)), unit)) {
            break;
        }
        if (unit == 0) {
            break;
        }

        char32_t cp = unit;
        // Mario Kart Wii's own text is BMP-only, but a well-formed surrogate pair is cheap to
        // decode and beats emitting two broken code points.
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < limit) {
            std::uint16_t low = 0;
            if (TryRead16(guestAddr + static_cast<std::uint32_t>((i + 1) * sizeof(std::uint16_t)),
                          low) &&
                low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((static_cast<char32_t>(unit - 0xD800) << 10) | (low - 0xDC00));
                ++i;
            } else {
                continue;  // Unpaired surrogate: not text.
            }
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
            continue;
        }

        if (IsUnspeakable(cp)) {
            // Keep a word boundary where the tag was, so neighbouring words do not merge.
            utf8.push_back(' ');
            continue;
        }
        AppendUtf8(utf8, cp);
    }

    return Tidy(utf8);
}

}  // namespace a11y
