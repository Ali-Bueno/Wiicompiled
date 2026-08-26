#ifndef MKW_ACCESSIBILITY_GUEST_TEXT_H
#define MKW_ACCESSIBILITY_GUEST_TEXT_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace a11y {

// Upper bound on characters read from one guest string. nw4r allocates the real length through
// TextBox::AllocStringBuffer, which we cannot see from a hook, so this is a defensive cap against
// an unterminated or corrupt buffer walking guest memory. Chosen well above any Mario Kart Wii menu
// string; raise it if a legitimate string is ever seen truncated.
inline constexpr std::size_t kMaxGuestTextChars = 512;

// Reads a UTF-16BE string from guest memory and returns UTF-8 suitable for a screen reader.
//
// nw4r's wchar_t is 16 bits and guest memory is big-endian. Mario Kart Wii also substitutes BMG
// tags (player names, numbers, button icons) into the composed string as private-use and control
// code points, which a screen reader would either skip or read as garbage, so those are dropped and
// the surrounding whitespace collapsed.
//
// Reading stops at the first NUL or after maxChars, whichever comes first, so a caller that already
// knows the length (the 3-argument TextBox::SetString overload passes one) just passes it here.
// Returns an empty string if the address is unmapped or the text is blank. Never throws.
std::string ReadGuestText(std::uint32_t guestAddr,
                          std::size_t maxChars = kMaxGuestTextChars) noexcept;

// Same conversion and sanitising, for UTF-16 that is already in host memory - the text captured
// character by character as the game composes it.
std::string Utf16ToUtf8(const std::u16string& text) noexcept;

}  // namespace a11y

#endif  // MKW_ACCESSIBILITY_GUEST_TEXT_H
