#ifndef MKW_ACCESSIBILITY_GUEST_TEXT_H
#define MKW_ACCESSIBILITY_GUEST_TEXT_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace a11y {

// Defensive cap for unterminated guest buffers, above normal MKWii menu-string lengths.
inline constexpr std::size_t kMaxGuestTextChars = 512;

// Reads UTF-16BE through NUL or maxChars; strips BMG glyphs and trims/collapses whitespace.
// Unmapped memory ends the read. Allocation failure returns empty text; never throws.
std::string ReadGuestText(std::uint32_t guestAddr,
                          std::size_t maxChars = kMaxGuestTextChars) noexcept;

// The same sanitising for host UTF-16. Unpaired surrogates are skipped; never throws.
std::string Utf16ToUtf8(const std::u16string& text) noexcept;

// Mii names are UCS-2: drops controls, non-BMP characters and malformed UTF-8; never throws.
std::u16string Utf8ToUtf16(const std::string& text) noexcept;

}  // namespace a11y

#endif  // MKW_ACCESSIBILITY_GUEST_TEXT_H
