#ifndef MKW_ACCESSIBILITY_RACE_GUEST_READ_H
#define MKW_ACCESSIBILITY_RACE_GUEST_READ_H

#include <cmath>
#include <cstdint>
#include <cstring>

#include "memory.h"

namespace a11y::race {

// Guest pointers are 32-bit, so an array of them strides by four. Defined here rather than per
// file because the build is a unity build: separate translation units are concatenated into one,
// which merges their anonymous namespaces and turns a repeated constant into a redefinition.
inline constexpr std::uint32_t kPointerStride = 4;

// Non-throwing guest reads. Memory::Read8 and ReadFloat32 throw on a bad address and a mod must
// never take the game down, so everything here goes through TryRead32 or a Contains guard.
//
// TryRead32 already byte-swaps big-endian guest memory into host order. That is what decides which
// half of a word a narrower field lands in, so the halfword and byte helpers below exist rather
// than being open-coded at each call site and getting it wrong once.

inline bool TryFloat(std::uint32_t addr, float& out) {
    std::uint32_t bits = 0;
    if (!Memory::TryRead32(addr, bits)) {
        return false;
    }
    std::memcpy(&out, &bits, sizeof(out));
    return std::isfinite(out);
}

inline bool TryVec3(std::uint32_t addr, float& x, float& y, float& z) {
    return TryFloat(addr, x) && TryFloat(addr + 4, y) && TryFloat(addr + 8, z);
}

inline bool TryPointer(std::uint32_t addr, std::uint32_t& out) {
    return Memory::TryRead32(addr, out) && out != 0;
}

// The only write the mod makes to guest memory. TryWrite32 is the non-throwing form, and it
// byte-swaps host to big-endian exactly as TryRead32 swaps the other way, so handing it the float's
// bit pattern round-trips correctly.
inline bool TryWriteFloat(std::uint32_t addr, float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return Memory::TryWrite32(addr, bits);
}

// A byte, addressed by the aligned word containing it, for the same reason the halfword helper is:
// Read8 throws, and Contains is not a guarantee the read itself will succeed.
inline bool TryU8(std::uint32_t addr, std::uint8_t& out) {
    std::uint32_t word = 0;
    if (!Memory::TryRead32(addr & ~std::uint32_t{3}, word)) {
        return false;
    }
    // After the swap to host order the byte at the word's own address is the most significant one.
    const std::uint32_t shift = (3u - (addr & 3u)) * 8u;
    out = static_cast<std::uint8_t>((word >> shift) & 0xFFu);
    return true;
}

// A halfword, addressed by the aligned word containing it. `highHalf` picks which of the two it is:
// after the swap to host order, the halfword at the word's own address is the high one.
inline bool TryU16(std::uint32_t wordAddr, bool highHalf, std::uint16_t& out) {
    std::uint32_t word = 0;
    if (!Memory::TryRead32(wordAddr, word)) {
        return false;
    }
    out = static_cast<std::uint16_t>(highHalf ? (word >> 16) : (word & 0xFFFFu));
    return true;
}

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_GUEST_READ_H
