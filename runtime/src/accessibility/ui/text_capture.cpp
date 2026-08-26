#include "text_capture.h"

#include <unordered_map>

#include "accessibility/guest_text.h"
#include "memory.h"

namespace a11y::ui {
namespace {

// Text::PaneHandler::Init (func_805CDBE4) writes the nw4r TextBox pointer at offset 0.
constexpr std::uint32_t kPaneHandlerTextBox = 0;

// A composed label is short; this only guards against a runaway or misread stream.
constexpr std::size_t kMaxComposedChars = 512;

// The string being composed right now, and the pane it belongs to.
std::uint32_t g_pendingTextBox = 0;
std::u16string g_pending;

// Last complete string seen for each TextBox. Committed rather than overwritten in place: the game
// announces the start of a pane's text far more often than it actually composes characters, so
// clearing on every announcement would leave every entry empty exactly when it is asked for.
std::unordered_map<std::uint32_t, std::u16string> g_committed;

void CommitPending() {
    if (g_pendingTextBox != 0 && !g_pending.empty()) {
        g_committed[g_pendingTextBox] = g_pending;
    }
    g_pending.clear();
}

}  // namespace

void BeginPaneText(std::uint32_t paneHandler) {
    CommitPending();

    std::uint32_t textBox = 0;
    if (paneHandler == 0 || !Memory::TryRead32(paneHandler + kPaneHandlerTextBox, textBox) ||
        textBox == 0) {
        g_pendingTextBox = 0;
        return;
    }
    g_pendingTextBox = textBox;
}

void AppendPaneChar(std::uint16_t codeUnit) {
    if (g_pendingTextBox == 0 || codeUnit == 0) {
        return;
    }
    if (g_pending.size() < kMaxComposedChars) {
        g_pending.push_back(static_cast<char16_t>(codeUnit));
    }
}

std::string TextForTextBox(std::uint32_t textBox) noexcept {
    // The pane being composed right now has not been committed yet.
    if (textBox == g_pendingTextBox && !g_pending.empty()) {
        return Utf16ToUtf8(g_pending);
    }
    const auto it = g_committed.find(textBox);
    if (it == g_committed.end() || it->second.empty()) {
        return {};
    }
    return Utf16ToUtf8(it->second);
}

void ClearCapturedText() {
    // Deliberately does not drop the committed strings. A pane keeps its text for as long as it
    // lives, and losing it means falling back to the untranslated .brlyt buffer - much worse than
    // briefly holding a string for a pointer the game has recycled.
    g_pending.clear();
    g_pendingTextBox = 0;
}

}  // namespace a11y::ui
