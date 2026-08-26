#include "lyt_walk.h"

#include <unordered_set>
#include <vector>

#include "accessibility/a11y_log.h"
#include "memory.h"
#include "text_capture.h"

namespace a11y::ui {
namespace {

// Structure offsets recovered from the translated code. There are no headers for these types, so
// each one cites the function that proves it (see CLAUDE.md section 9 - never a bare literal).

// LayoutUIControl::LoadLayout (func_8063D954) reads the layout's root pane from this+188 right
// after MainLayout::Build, and MessageRequester passes the same field to PaneManager::DoAction.
constexpr std::uint32_t kControlRootPane = 188;

// nw4r::lyt::Pane, from Pane::FindPaneByName (func_80078D80) and PaneManager::SearchForPane
// (func_805E7460): the child list at +20 is a circular list whose sentinel is the field itself, and
// each node sits at child+4, so a node maps back to its pane by subtracting 4.
constexpr std::uint32_t kPaneChildListHead = 20;
constexpr std::uint32_t kPaneLinkInNode = 4;
constexpr std::uint32_t kPaneParent = 12;

// Visibility, from PaneManager::IsPaneAndParentsVisible (func_805E7700): it tests bit 0 of the flag
// byte at +187 and walks up through +12, so a pane is visible only if it and every ancestor are.
// Bit 1 of the same byte is a different flag (LayoutUIControl::LoadLayout sets it, and
// IsPaneAndParentsVisible never reads it) - do not mistake it for visibility.
constexpr std::uint32_t kPaneFlags = 187;
constexpr std::uint8_t kPaneFlagVisible = 0x01;

// Pane::CalculateMtx (func_80078EF0) keeps the pane's own alpha at +184. The effective alpha at
// +185 is only refreshed while the pane is being drawn, so it goes stale on hidden panes and is not
// safe to test; an explicit own-alpha of zero is unambiguous.
constexpr std::uint32_t kPaneAlpha = 184;

// The nw4r TextBox string buffer (*(this+216), length at +254) is deliberately not read here: it
// holds the untranslated .brlyt placeholder, never what the player sees. See text_capture.h.

// Guard rails: a corrupt or mid-rebuild tree must not spin. Mario Kart Wii's control layouts are a
// handful of panes deep, so these are far above anything legitimate.
constexpr std::size_t kMaxPanesVisited = 512;
constexpr std::size_t kMaxDepth = 32;

// Learned at runtime from live TextBox instances rather than hardcoding nw4r's vtable address.
std::unordered_set<std::uint32_t>& TextBoxVtables() {
    static std::unordered_set<std::uint32_t> vtables;
    return vtables;
}

bool TryRead16(std::uint32_t addr, std::uint16_t& value) noexcept {
    try {
        value = Memory::Read16(addr);
        return true;
    } catch (const Memory::AccessViolation&) {
        return false;
    }
}

bool TryRead8(std::uint32_t addr, std::uint8_t& value) noexcept {
    try {
        value = Memory::Read8(addr);
        return true;
    } catch (const Memory::AccessViolation&) {
        return false;
    }
}

// One pane in isolation. The walk descends from a pane whose ancestors are already known visible,
// so checking each pane on the way down and pruning its subtree is equivalent to the game's own
// walk-up test, at a fraction of the reads.
bool IsPaneSelfVisible(std::uint32_t pane) noexcept {
    std::uint8_t flags = 0;
    if (!TryRead8(pane + kPaneFlags, flags) || (flags & kPaneFlagVisible) == 0) {
        return false;
    }
    std::uint8_t alpha = 0;
    if (!TryRead8(pane + kPaneAlpha, alpha) || alpha == 0) {
        return false;
    }
    return true;
}

// The control's own layout root can still be hidden by something above it.
bool AreAncestorsVisible(std::uint32_t pane) noexcept {
    std::uint32_t parent = 0;
    if (!Memory::TryRead32(pane + kPaneParent, parent)) {
        return true;
    }
    for (std::size_t depth = 0; parent != 0 && depth < kMaxDepth; ++depth) {
        if (!IsPaneSelfVisible(parent)) {
            return false;
        }
        if (!Memory::TryRead32(parent + kPaneParent, parent)) {
            break;
        }
    }
    return true;
}

bool IsKnownTextBox(std::uint32_t pane) noexcept {
    std::uint32_t vtable = 0;
    if (!Memory::TryRead32(pane, vtable) || vtable == 0) {
        return false;
    }
    return TextBoxVtables().count(vtable) != 0;
}

}  // namespace

void NoteTextBoxInstance(std::uint32_t textBox) {
    std::uint32_t vtable = 0;
    if (textBox == 0 || !Memory::TryRead32(textBox, vtable) || vtable == 0) {
        return;
    }
    if (TextBoxVtables().insert(vtable).second) {
        RT_LOGF(RT_TAG_A11Y, "learned TextBox vtable %08x\n", vtable);
    }
}

std::string ReadControlText(std::uint32_t control) noexcept {
    std::uint32_t root = 0;
    if (control == 0 || !Memory::TryRead32(control + kControlRootPane, root) || root == 0) {
        return {};
    }
    if (!IsPaneSelfVisible(root) || !AreAncestorsVisible(root)) {
        return {};
    }

    std::string result;
    std::vector<std::pair<std::uint32_t, std::size_t>> stack{{root, 0}};
    std::vector<std::uint32_t> children;
    std::size_t visited = 0;

    while (!stack.empty() && visited < kMaxPanesVisited) {
        const auto [pane, depth] = stack.back();
        stack.pop_back();
        ++visited;

        const bool selfVisible = IsPaneSelfVisible(pane);


        // Hidden panes keep their untranslated placeholder text, so reading one means speaking
        // something the player cannot see. Skipping the whole subtree is correct: the game's own
        // test requires every ancestor to be visible too.
        if (!selfVisible) {
            continue;
        }

        if (IsKnownTextBox(pane)) {
            // Only the composed string counts. A pane the Text:: system never wrote to is not
            // showing text at all - Mario Kart Wii leaves the untranslated .brlyt placeholder in
            // the nw4r buffer and simply does not draw it, so falling back to that buffer reads
            // out template junk the player cannot see (Japanese names, unused class labels).
            const std::string text = TextForTextBox(pane);
            // Buttons carry shadow and highlight copies of the same label, so the same string
            // legitimately appears several times under one control.
            if (!text.empty() && result.find(text) == std::string::npos) {
                if (!result.empty()) {
                    result += ", ";
                }
                result += text;
            }
        }

        if (depth >= kMaxDepth) {
            continue;
        }
        const std::uint32_t sentinel = pane + kPaneChildListHead;
        std::uint32_t node = 0;
        if (!Memory::TryRead32(sentinel, node)) {
            continue;
        }
        // Collect first, then push reversed: a stack pops last-in-first, and reading a menu back to
        // front would break the on-screen order the spec requires.
        children.clear();
        while (node != 0 && node != sentinel && children.size() < kMaxPanesVisited) {
            children.push_back(node - kPaneLinkInNode);
            std::uint32_t next = 0;
            if (!Memory::TryRead32(node, next)) {
                break;
            }
            node = next;
        }
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            stack.emplace_back(*it, depth + 1);
        }
    }

    return result;
}

}  // namespace a11y::ui
