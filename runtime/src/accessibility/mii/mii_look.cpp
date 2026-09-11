#include "accessibility/mii/mii_look.h"

#include "memory.h"

namespace a11y::mii {
namespace {

// FNV-1a of the name seeds a xorshift64; deterministic on purpose.
class NameRandom {
public:
    NameRandom(const Name& name, std::uint32_t variant) {
        state_ = 14695981039346656037ull;
        for (const char16_t unit : name) {
            state_ ^= unit;
            state_ *= 1099511628211ull;
        }
        state_ ^= variant;
        state_ *= 1099511628211ull;
        if (state_ == 0) {
            state_ = 1;
        }
    }

    std::uint64_t Next() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 7;
        state_ ^= state_ << 17;
        return state_;
    }

    std::uint32_t NextLimited(std::uint32_t max) {
        return static_cast<std::uint32_t>((Next() >> 33) % (static_cast<std::uint64_t>(max) + 1));
    }

private:
    std::uint64_t state_;
};

// Replaces one bit run of a record field, leaving the rest of the word as it was. Memory's
// accessors already byte-swap, so the bit numbering is applied on the host-order value.
void SetField(std::uint32_t entry, const BitField& field, std::uint32_t value) {
    const std::uint32_t addr = entry + field.offset;
    const std::uint32_t shift = field.wordBits - field.firstBit - field.bits;
    const std::uint32_t mask = ((1u << field.bits) - 1u) << shift;
    const std::uint32_t bits = (value << shift) & mask;
    switch (field.wordBits) {
        case 8:
            Memory::Write8(addr, static_cast<std::uint8_t>((Memory::Read8(addr) & ~mask) | bits));
            break;
        case 16:
            Memory::Write16(addr,
                            static_cast<std::uint16_t>((Memory::Read16(addr) & ~mask) | bits));
            break;
        default:
            Memory::Write32(addr, (Memory::Read32(addr) & ~mask) | bits);
            break;
    }
}

}  // namespace

void StampLook(std::uint32_t entry, const Name& name, std::uint32_t variant) {
    NameRandom random(name, variant);
    for (const BitField& field : kLookFields) {
        SetField(entry, field, random.NextLimited(field.max));
    }
}

CreateId CreateIdFor(const Name& name) {
    // The variant keeps this stream apart from the face's, so the id does not echo a field.
    NameRandom random(name, kDefaultCount);
    CreateId id{};
    for (std::uint8_t& byte : id) {
        do {
            byte = static_cast<std::uint8_t>(random.Next() >> 56);
        } while (byte == 0);  // every byte non-zero: never all-zero, never a built-in id
    }
    id[0] &= static_cast<std::uint8_t>(~kCreateIdSpecialBits);
    if (id[0] == 0) {
        id[0] = 1;
    }
    return id;
}

}  // namespace a11y::mii
