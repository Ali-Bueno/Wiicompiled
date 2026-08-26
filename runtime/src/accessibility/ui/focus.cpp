#include "focus.h"

#include <cstddef>

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
// manip+36 and +40, and ControlManipulator::Init (func_805EFC48) for the bounding-box array and its
// count. SetAction (func_805EFCF8) writes nine action handlers at manip + 64 + 4*index; only the
// first is needed here, since every one of them leads back to the same control.
constexpr std::uint32_t kManipulatorOnSelect = 36;
constexpr std::uint32_t kManipulatorOnDeselect = 40;
constexpr std::uint32_t kManipulatorBoundingBoxes = 52;
constexpr std::uint32_t kManipulatorBoundingBoxCount = 56;
constexpr std::uint32_t kManipulatorActionHandlers = 64;

// ControlsManipulatorManager embeds an EGG::List of every manipulator registered with it at +0x10 -
// AddControlManipulator (func_805F0D44) hands this+16 to EGG::List::Add. EGG::List::__ct
// (func_8022F760) lays the object out as a vtable then the head pointer, and there is no count
// anywhere: EGG::List::Add appends and the walk ends on a null link.
constexpr std::uint32_t kManagerManipulatorList = 0x10;
constexpr std::uint32_t kListHead = 0x04;

// ControlManipulator derives from EGG::Link at offset zero - its constructor (func_805EFAF8) runs
// EGG::Link::__ct on this+0 before installing its own vtable - so the next node is at +0.
constexpr std::uint32_t kManipulatorNext = 0x00;

// AddControlManipulator (func_805F0D44) writes the owning manager here and nothing else ever does,
// which is what makes the list walk safe without hardcoding a vtable: a node that does not point
// back at the manager it was reached from is not a manipulator.
constexpr std::uint32_t kManipulatorOwningManager = 60;

// The list carries no count, so the walk brings its own bound. A page's manipulators run to a few
// dozen - a character grid is the biggest at around thirty.
constexpr std::size_t kMaxManipulators = 256;

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

// The UIControl a manipulator drives.
//
// Plain buttons carry their handlers on the manipulator itself. Composite controls - the up/down
// arrows, the paged sheets - put one per bounding box instead, so a box's handler is the one that
// names the control. `boxIndex` selects which; a negative index means the manipulator has no focused
// box and the boxes are skipped. Any box leads to the same UIControl, since a manipulator's boxes
// are sub-areas of one control, so a caller that only wants the owner may pass zero.
std::uint32_t ControlOfManipulator(std::uint32_t manipulator, std::int32_t boxIndex) noexcept {
    if (!IsPlausiblePointer(manipulator)) {
        return 0;
    }
    std::uint32_t handler = 0;
    Memory::TryRead32(manipulator + kManipulatorOnSelect, handler);
    if (handler == 0) {
        Memory::TryRead32(manipulator + kManipulatorOnDeselect, handler);
    }
    if (handler == 0 && boxIndex >= 0) {
        std::uint32_t boxes = 0;
        std::uint32_t boxCount = 0;
        // Plain buttons are loaded with zero boxes (PushButton::Load passes count 0), so the array
        // pointer alone proves nothing - the count is what says the index is real.
        if (Memory::TryRead32(manipulator + kManipulatorBoundingBoxCount, boxCount) &&
            static_cast<std::uint32_t>(boxIndex) < boxCount &&
            Memory::TryRead32(manipulator + kManipulatorBoundingBoxes, boxes) &&
            IsPlausiblePointer(boxes)) {
            const std::uint32_t box =
                boxes + static_cast<std::uint32_t>(boxIndex) * kBoundingBoxStride;
            Memory::TryRead32(box + kBoundingBoxOnSelect, handler);
            if (handler == 0) {
                Memory::TryRead32(box + kBoundingBoxOnDeselect, handler);
            }
        }
    }
    if (handler == 0) {
        Memory::TryRead32(manipulator + kManipulatorActionHandlers, handler);
    }
    return OwnerOfHandler(handler);
}

// Whether a control really belongs to this page, so focus left over from the screen underneath is
// not read while a new one settles.
//
// The walk climbs, it does not check one level. A grid's buttons belong to the grid's own
// ControlGroup, not the page's, so that group's owner is the container control rather than a Page -
// checking a single level rejected every character and kart button as if it were another screen's.
bool BelongsToPage(std::uint32_t control, std::uint32_t page) noexcept {
    std::uint32_t owner = control;
    for (int level = 0; level < kMaxControlNesting; ++level) {
        std::uint32_t group = 0;
        if (!Memory::TryRead32(owner + kControlParentGroup, group) || !IsPlausiblePointer(group)) {
            return false;
        }
        if (!Memory::TryRead32(group + kControlGroupPage, owner) || !IsPlausiblePointer(owner)) {
            return false;
        }
        if (owner == page) {
            return true;
        }
    }
    return false;
}

std::uint32_t ManipulatorManager(std::uint32_t page) noexcept {
    std::uint32_t manager = 0;
    if (page == 0 || !Memory::TryRead32(page + kPageManipulatorManager, manager) ||
        !IsPlausiblePointer(manager)) {
        return 0;
    }
    return manager;
}

}  // namespace

std::uint32_t FocusedControl(std::uint32_t page) noexcept {
    const std::uint32_t manager = ManipulatorManager(page);
    if (manager == 0) {
        return 0;
    }

    const std::uint32_t holder = manager + kHolderArray + kHolderStride * kPlayerOneHolder;
    std::uint32_t manipulator = 0;
    if (!Memory::TryRead32(holder + kHolderFocusedManipulator, manipulator) ||
        !IsPlausiblePointer(manipulator)) {
        return 0;
    }
    std::uint32_t boxIndex = 0;
    if (!Memory::TryRead32(holder + kHolderFocusedBoxIndex, boxIndex)) {
        boxIndex = static_cast<std::uint32_t>(-1);
    }

    const std::uint32_t control =
        ControlOfManipulator(manipulator, static_cast<std::int32_t>(boxIndex));
    if (control == 0) {
        return 0;
    }
    return BelongsToPage(control, page) ? control : 0;
}

std::vector<std::uint32_t> SelectableControls(std::uint32_t page) noexcept {
    std::vector<std::uint32_t> controls;
    const std::uint32_t manager = ManipulatorManager(page);
    std::uint32_t node = 0;
    if (manager == 0 ||
        !Memory::TryRead32(manager + kManagerManipulatorList + kListHead, node)) {
        return controls;
    }

    // Every manipulator the page registered, which is the same set CheckActions hit-tests each
    // frame - so it cannot drift from what the cursor can actually reach.
    for (std::size_t visited = 0; node != 0; ++visited) {
        std::uint32_t owner = 0;
        if (visited == kMaxManipulators || !IsPlausiblePointer(node) ||
            !Memory::TryRead32(node + kManipulatorOwningManager, owner) || owner != manager) {
            // A link that leads somewhere else means the rest of the walk cannot be trusted, and a
            // half-read set would quietly demote real buttons to labels. Report nothing instead:
            // the caller treats an empty set on a focused page as a failed read.
            return {};
        }
        // Index zero because only the owner is wanted, and a manipulator's boxes are sub-areas of
        // one control. A manipulator with no boxes falls through to its own handlers.
        const std::uint32_t control = ControlOfManipulator(node, 0);
        if (control != 0) {
            controls.push_back(control);
        }
        if (!Memory::TryRead32(node + kManipulatorNext, node)) {
            return {};
        }
    }
    return controls;
}

}  // namespace a11y::ui
