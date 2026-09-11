#ifndef MKW_ACCESSIBILITY_MII_MII_LOOK_H
#define MKW_ACCESSIBILITY_MII_MII_LOOK_H

#include <cstdint>

#include "accessibility/mii/mii_guest.h"

// What a Mii with a given name looks like and which createID it carries. Both are derived from
// the name alone, so the same name is the same Mii on every launch and other players recognise it.
namespace a11y::mii {

// Rewrites the type and colour fields of the 74-byte record at `entry` (see kLookFields).
// `variant` picks a different face for the same name. Throws Memory::AccessViolation.
void StampLook(std::uint32_t entry, const Name& name, std::uint32_t variant);

// A createID of our own: never one of the built-in ones and never flagged special, so no console
// substitutes "Player" for the name.
CreateId CreateIdFor(const Name& name);

}  // namespace a11y::mii

#endif  // MKW_ACCESSIBILITY_MII_MII_LOOK_H
