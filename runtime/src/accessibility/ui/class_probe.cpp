#include "class_probe.h"

#include <unordered_map>

#include "memory.h"

namespace a11y::ui {
namespace {

// A bound on a search, not a value read from the game: MKWii's UIControl vtables are far shorter.
// Only the primary vtable at the object's own address is scanned - a class with more than one base
// keeps a second table further along, which says nothing about the class's identity.
constexpr std::uint32_t kMaxVtableSlots = 128;

}  // namespace

bool ImplementsMethod(std::uint32_t object, std::uint32_t method) {
    std::uint32_t vtable = 0;
    if (!Memory::TryRead32(object, vtable) || vtable == 0) {
        return false;
    }

    static std::unordered_map<std::uint64_t, bool> answered;
    const std::uint64_t question = (static_cast<std::uint64_t>(vtable) << 32) | method;
    const auto seen = answered.find(question);
    if (seen != answered.end()) {
        return seen->second;
    }

    bool found = false;
    for (std::uint32_t slot = 0; slot < kMaxVtableSlots && !found; ++slot) {
        std::uint32_t entry = 0;
        if (!Memory::TryRead32(vtable + slot * sizeof(std::uint32_t), entry)) {
            break;
        }
        found = entry == method;
    }
    answered[question] = found;
    return found;
}

}  // namespace a11y::ui
