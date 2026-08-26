#include "focus.h"

#include "memory.h"

namespace a11y::ui {
namespace {

// Page::SetManipulatorManager (func_80602474) writes the manager pointer here; it is a pointer, not
// an embedded object.
constexpr std::uint32_t kPageManipulatorManager = 0x38;

// ControlsManipulatorManager::Init (func_805F0C48) walks five ControlManipulatorHolder objects
// starting at this+84, each 92 bytes. Update (func_805F1F40) pairs holder i with controller i, so
// player one is holder zero.
constexpr std::uint32_t kHolderArray = 84;
constexpr std::uint32_t kHolderStride = 92;
constexpr std::uint32_t kPlayerOneHolder = 0;

// CheckActions (func_805F0E94) writes exactly these two fields when the cursor moves: the focused
// ControlManipulator and the index of the focused bounding box within it (-1 when it has none).
constexpr std::uint32_t kHolderFocusedManipulator = 0x00;
constexpr std::uint32_t kHolderFocusedBoxIndex = 0x04;

// ControlManipulator, from PushButton::Load (func_805BD518) writing the button's own handlers into
// manip+36 and +40, and ControlManipulator::Init (func_805EFC48) for the bounding-box array.
constexpr std::uint32_t kManipulatorOnSelect = 36;
constexpr std::uint32_t kManipulatorOnDeselect = 40;
constexpr std::uint32_t kManipulatorBoundingBoxes = 52;
constexpr std::uint32_t kManipulatorActionHandlers = 64;

// ControlBoundingBox is 40 bytes with its select/deselect handlers at +24 and +28
// (ControlBoundingBox::TriggerOnSelectHandler, func_805F08A0).
constexpr std::uint32_t kBoundingBoxStride = 40;
constexpr std::uint32_t kBoundingBoxOnSelect = 24;
constexpr std::uint32_t kBoundingBoxOnDeselect = 28;

// A Handler carries the object that owns it at +4; PushButton::__ct (func_805BD3A8) writes `this`
// there. That is how a focused manipulator leads back to a UIControl.
constexpr std::uint32_t kHandlerOwner = 4;

// UIControl::AddControl (func_8063D278) and ControlGroup::SetControl (func_805C27DC): a control
// knows its parent group, and a group knows its page. Used to reject focus left over from a screen
// that is no longer on top.
constexpr std::uint32_t kControlParentGroup = 100;
constexpr std::uint32_t kControlGroupPage = 12;

// A grid inside a page inside nothing deeper than this in practice; the bound only stops a cycle in
// corrupt memory from spinning.
constexpr int kMaxControlNesting = 8;

// The manipulator is mid-update for a frame or two while a screen changes, so these fields can hold
// a value that is not a pointer at all - a run showed 0x31010000 come back as a "control". Reading a
// struct off that address would be reading noise, and noise reads aloud.
bool IsPlausiblePointer(std::uint32_t address) noexcept {
    return address != 0 && Memory::Contains(address, sizeof(std::uint32_t));
}

std::uint32_t OwnerOfHandler(std::uint32_t handler) noexcept {
    std::uint32_t owner = 0;
    if (!IsPlausiblePointer(handler) || !Memory::TryRead32(handler + kHandlerOwner, owner)) {
        return 0;
    }
    return IsPlausiblePointer(owner) ? owner : 0;
}

}  // namespace

std::uint32_t FocusedControl(std::uint32_t page) noexcept {
    std::uint32_t manager = 0;
    if (page == 0 || !Memory::TryRead32(page + kPageManipulatorManager, manager) ||
        !IsPlausiblePointer(manager)) {
        return 0;
    }

    const std::uint32_t holder = manager + kHolderArray + kHolderStride * kPlayerOneHolder;
    std::uint32_t manipulator = 0;
    if (!Memory::TryRead32(holder + kHolderFocusedManipulator, manipulator) ||
        !IsPlausiblePointer(manipulator)) {
        return 0;
    }

    // Plain buttons carry their handlers on the manipulator itself. Composite controls - the
    // up/down arrows, the paged sheets - put one per bounding box instead, so the focused box's
    // handler is the one that names the control.
    std::uint32_t handler = 0;
    Memory::TryRead32(manipulator + kManipulatorOnSelect, handler);
    if (handler == 0) {
        Memory::TryRead32(manipulator + kManipulatorOnDeselect, handler);
    }
    if (handler == 0) {
        std::uint32_t boxIndex = 0;
        std::uint32_t boxes = 0;
        if (Memory::TryRead32(holder + kHolderFocusedBoxIndex, boxIndex) &&
            static_cast<std::int32_t>(boxIndex) >= 0 &&
            Memory::TryRead32(manipulator + kManipulatorBoundingBoxes, boxes) &&
            IsPlausiblePointer(boxes)) {
            const std::uint32_t box = boxes + boxIndex * kBoundingBoxStride;
            Memory::TryRead32(box + kBoundingBoxOnSelect, handler);
            if (handler == 0) {
                Memory::TryRead32(box + kBoundingBoxOnDeselect, handler);
            }
        }
    }
    if (handler == 0) {
        Memory::TryRead32(manipulator + kManipulatorActionHandlers, handler);
    }

    const std::uint32_t control = OwnerOfHandler(handler);
    if (control == 0) {
        return 0;
    }

    // The manipulator can still point at a control from the screen underneath while a new one is
    // settling, so the control has to be tied back to this page before it is trusted.
    //
    // The walk climbs, it does not check one level. A grid's buttons belong to the grid's own
    // ControlGroup, not the page's, so that group's owner is the container control rather than a
    // Page - checking a single level rejected every character and kart button as if it belonged to
    // another screen.
    std::uint32_t owner = control;
    for (int level = 0; level < kMaxControlNesting; ++level) {
        std::uint32_t group = 0;
        if (!Memory::TryRead32(owner + kControlParentGroup, group) || !IsPlausiblePointer(group)) {
            break;
        }
        if (!Memory::TryRead32(group + kControlGroupPage, owner) || !IsPlausiblePointer(owner)) {
            break;
        }
        if (owner == page) {
            return control;
        }
    }
    return 0;
}

}  // namespace a11y::ui
