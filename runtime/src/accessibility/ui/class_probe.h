#ifndef MKW_ACCESSIBILITY_UI_CLASS_PROBE_H
#define MKW_ACCESSIBILITY_UI_CLASS_PROBE_H

#include <cstdint>

namespace a11y::ui {

// Whether an object's class implements a given method, answered by looking for that method's address
// in the object's vtable.
//
// A live object's vtable holds the original guest addresses, so a symbol out of MAP.txt names a
// class without hardcoding a vtable address - the same "ask the game" test the rest of the reader
// uses, and the reason a select screen can be recognised by the buttons on it rather than by which
// page it is. Answers are cached per class, so the scan happens once.
bool ImplementsMethod(std::uint32_t object, std::uint32_t method);

// Page::OnActivate's offset in every page vtable (StaticR: Options 0x808BA3EC, LicenseSelect
// 0x808B984C).
constexpr std::uint32_t kPageOnActivateSlot = 0x30;

// The exact-slot test pages need: their vtables sit back to back in StaticR, so a scan from one
// runs into its neighbour's slots (docs/options-entry.md, Traps).
bool HasMethodAtSlot(std::uint32_t object, std::uint32_t slotOffset, std::uint32_t method);

}  // namespace a11y::ui

#endif  // MKW_ACCESSIBILITY_UI_CLASS_PROBE_H
