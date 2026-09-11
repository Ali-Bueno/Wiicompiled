#ifndef MKW_ACCESSIBILITY_MII_MII_GUEST_H
#define MKW_ACCESSIBILITY_MII_MII_GUEST_H

#include <array>
#include <cstddef>
#include <cstdint>

#include "accessibility/mii/mii_data.h"
#include "memory.h"

// Guest reads and writes for Mii records that stop at a bad address instead of throwing out of
// the frame tick. Memory's accessors byte-swap, so a name unit or a word round-trips in host order.
namespace a11y::mii {

using Name = std::array<char16_t, kNameUnits>;  // zero padded, like the record
using CreateId = std::array<std::uint8_t, kCreateIdSize>;

inline bool ReadName(std::uint32_t addr, Name& out) noexcept {
    try {
        for (std::size_t k = 0; k < out.size(); ++k) {
            out[k] = static_cast<char16_t>(Memory::Read16(addr + 2 * k));
        }
        return true;
    } catch (const Memory::AccessViolation&) {
        return false;
    }
}

// `terminate` writes a NUL after the ten units where the field has room for one.
inline bool WriteName(std::uint32_t addr, const Name& name, bool terminate) noexcept {
    try {
        for (std::size_t k = 0; k < name.size(); ++k) {
            Memory::Write16(addr + 2 * k, static_cast<std::uint16_t>(name[k]));
        }
        if (terminate) {
            Memory::Write16(addr + 2 * name.size(), 0);
        }
        return true;
    } catch (const Memory::AccessViolation&) {
        return false;
    }
}

inline bool ReadCreateId(std::uint32_t addr, CreateId& out) noexcept {
    try {
        for (std::size_t k = 0; k < out.size(); ++k) {
            out[k] = Memory::Read8(addr + k);
        }
        return true;
    } catch (const Memory::AccessViolation&) {
        return false;
    }
}

inline bool WriteCreateId(std::uint32_t addr, const CreateId& id) noexcept {
    try {
        for (std::size_t k = 0; k < id.size(); ++k) {
            Memory::Write8(addr + k, id[k]);
        }
        return true;
    } catch (const Memory::AccessViolation&) {
        return false;
    }
}

inline bool CopyBytes(std::uint32_t from, std::uint32_t to, std::uint32_t count) noexcept {
    try {
        for (std::uint32_t k = 0; k < count; ++k) {
            Memory::Write8(to + k, Memory::Read8(from + k));
        }
        return true;
    } catch (const Memory::AccessViolation&) {
        return false;
    }
}

inline bool ReadU8(std::uint32_t addr, std::uint8_t& out) noexcept {
    try {
        out = Memory::Read8(addr);
        return true;
    } catch (const Memory::AccessViolation&) {
        return false;
    }
}

inline bool WriteU8(std::uint32_t addr, std::uint8_t value) noexcept {
    try {
        Memory::Write8(addr, value);
        return true;
    } catch (const Memory::AccessViolation&) {
        return false;
    }
}

}  // namespace a11y::mii

#endif  // MKW_ACCESSIBILITY_MII_MII_GUEST_H
