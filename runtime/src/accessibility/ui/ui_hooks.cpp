// Observation hooks on Mario Kart Wii's menu system.
//
// These use REGISTER_NATIVE_FUNCTION_AS, not PPC_NATIVE_OVERRIDE: _AS keeps the translated body, so
// func_<ADDR> still exists and the original behaviour runs unchanged. See CLAUDE.md section 3.
//
// Two rules for every wrapper here:
//   1. Capture the arguments BEFORE calling the original - r3-r12 are volatile and the callee
//      clobbers them.
//   2. No logic. Read arguments, call the original, hand the values to ui_events.

#include <cstdint>

#include "abi_bridge.h"
#include "accessibility/ui/lyt_walk.h"
#include "accessibility/ui/text_capture.h"
#include "accessibility/ui/ui_events.h"
#include "isa/ppc_isa_context.h"

// Addresses come from projects/mkwii/MAP.txt; the symbol each one is named after is in the comment.
extern "C" {
void func_8007C4C0(CpuContext* ctx);  // nw4r::lyt::TextBox::SetString(const wchar_t*, u16)
void func_8007C5A0(CpuContext* ctx);  // nw4r::lyt::TextBox::SetString(const wchar_t*, u16, u16)
void func_8063DCBC(CpuContext* ctx);  // LayoutUIControl::SetTextBoxMessage
void func_805BDFFC(CpuContext* ctx);  // PushButton::HandleSelect
void func_805BDAF0(CpuContext* ctx);  // PushButton::SelectInitial
void func_806029F4(CpuContext* ctx);  // Page::EndEntrance
void func_80601AEC(CpuContext* ctx);  // Page::Activate
void func_80635080(CpuContext* ctx);  // SectionMgr::EnterSection
void func_805CFC24(CpuContext* ctx);  // Text::GlobalHandler::SetPaneHandler
void func_805D0E08(CpuContext* ctx);  // Text::GlobalHandler::SetPaneHandlerAndPane
void func_805D00B0(CpuContext* ctx);  // Text::GlobalHandler::SetCharacter
void func_805D0324(CpuContext* ctx);  // Text::GlobalHandler::SetExternalUnicodeChar
}

extern "C" void MkwTextBoxSetString_Observe_8007c4c0(CpuContext* ctx) {
    const std::uint32_t self = ctx->gpr[3];
    func_8007C4C0(ctx);
    // The string itself is ignored: what reaches here is the Japanese default baked into the
    // .brlyt, not the localized text the player sees. All we want is a live instance to learn the
    // TextBox vtable from, so the pane walk can recognise one without a hardcoded address.
    a11y::ui::NoteTextBoxInstance(self);
}

extern "C" void MkwTextBoxSetStringLen_Observe_8007c5a0(CpuContext* ctx) {
    const std::uint32_t self = ctx->gpr[3];
    func_8007C5A0(ctx);
    a11y::ui::NoteTextBoxInstance(self);
}

extern "C" void MkwLayoutUIControlSetTextBoxMessage_Observe_8063dcbc(CpuContext* ctx) {
    // Left registered but passive. ControlLoader::LoadMessages only calls this once per BMG entry
    // at page-load time, so it is no use as a text source; r5 carries the BMG message id, which is
    // the stable language-independent label we will want later.
    func_8063DCBC(ctx);
}

extern "C" void MkwPushButtonHandleSelect_Observe_805bdffc(CpuContext* ctx) {
    const std::uint32_t control = ctx->gpr[3];

    func_805BDFFC(ctx);

    a11y::ui::OnControlSelected(control, /*initial=*/false);
}

extern "C" void MkwPushButtonSelectInitial_Observe_805bdaf0(CpuContext* ctx) {
    const std::uint32_t control = ctx->gpr[3];

    func_805BDAF0(ctx);

    a11y::ui::OnControlSelected(control, /*initial=*/true);
}

extern "C" void MkwPageEndEntrance_Observe_806029f4(CpuContext* ctx) {
    const std::uint32_t page = ctx->gpr[3];

    func_806029F4(ctx);

    a11y::ui::OnPageEntered(page);
}

extern "C" void MkwPageActivate_Observe_80601aec(CpuContext* ctx) {
    const std::uint32_t page = ctx->gpr[3];

    // Notified BEFORE the original, unlike every other hook here. Activate runs the page's
    // OnActivate, which is where a page gives its initial control focus - so the selection happens
    // *inside* this call. Registering the page afterwards would wipe the record of that selection
    // and leave the reader thinking nothing took focus, which made it recite whole menus, and it
    // would let a dialog's button speak before the message it belongs to.
    a11y::ui::OnPageActivated(page);

    func_80601AEC(ctx);
}

extern "C" void MkwSectionMgrEnterSection_Observe_80635080(CpuContext* ctx) {
    const std::uint32_t section = ctx->gpr[3];

    func_80635080(ctx);

    a11y::ui::OnSectionEntered(section);
}

// Mario Kart Wii composes the localized string here, one code unit at a time, and draws it itself.
// This is the only place the translated text exists - the nw4r TextBox buffer keeps the Japanese
// original from the .brlyt.
extern "C" void MkwTextSetPaneHandler_Observe_805cfc24(CpuContext* ctx) {
    const std::uint32_t paneHandler = ctx->gpr[4];
    func_805CFC24(ctx);
    a11y::ui::BeginPaneText(paneHandler);
}

extern "C" void MkwTextSetPaneHandlerAndPane_Observe_805d0e08(CpuContext* ctx) {
    const std::uint32_t paneHandler = ctx->gpr[4];
    func_805D0E08(ctx);
    a11y::ui::BeginPaneText(paneHandler);
}

extern "C" void MkwTextSetCharacter_Observe_805d00b0(CpuContext* ctx) {
    const std::uint16_t codeUnit = static_cast<std::uint16_t>(ctx->gpr[4]);
    func_805D00B0(ctx);
    a11y::ui::AppendPaneChar(codeUnit);
}

extern "C" void MkwTextSetExternalUnicodeChar_Observe_805d0324(CpuContext* ctx) {
    const std::uint16_t codeUnit = static_cast<std::uint16_t>(ctx->gpr[4]);
    func_805D0324(ctx);
    // Externally substituted characters - player names, numbers - come through here instead.
    a11y::ui::AppendPaneChar(codeUnit);
}

REGISTER_NATIVE_FUNCTION_AS(0x805CFC24, MkwTextSetPaneHandler_Observe_805cfc24,
                            "MkwTextSetPaneHandler_Observe_805cfc24");
REGISTER_NATIVE_FUNCTION_AS(0x805D0E08, MkwTextSetPaneHandlerAndPane_Observe_805d0e08,
                            "MkwTextSetPaneHandlerAndPane_Observe_805d0e08");
REGISTER_NATIVE_FUNCTION_AS(0x805D00B0, MkwTextSetCharacter_Observe_805d00b0,
                            "MkwTextSetCharacter_Observe_805d00b0");
REGISTER_NATIVE_FUNCTION_AS(0x805D0324, MkwTextSetExternalUnicodeChar_Observe_805d0324,
                            "MkwTextSetExternalUnicodeChar_Observe_805d0324");

REGISTER_NATIVE_FUNCTION_AS(0x8007C4C0, MkwTextBoxSetString_Observe_8007c4c0,
                            "MkwTextBoxSetString_Observe_8007c4c0");
REGISTER_NATIVE_FUNCTION_AS(0x8007C5A0, MkwTextBoxSetStringLen_Observe_8007c5a0,
                            "MkwTextBoxSetStringLen_Observe_8007c5a0");
REGISTER_NATIVE_FUNCTION_AS(0x8063DCBC, MkwLayoutUIControlSetTextBoxMessage_Observe_8063dcbc,
                            "MkwLayoutUIControlSetTextBoxMessage_Observe_8063dcbc");
REGISTER_NATIVE_FUNCTION_AS(0x805BDFFC, MkwPushButtonHandleSelect_Observe_805bdffc,
                            "MkwPushButtonHandleSelect_Observe_805bdffc");
REGISTER_NATIVE_FUNCTION_AS(0x805BDAF0, MkwPushButtonSelectInitial_Observe_805bdaf0,
                            "MkwPushButtonSelectInitial_Observe_805bdaf0");
REGISTER_NATIVE_FUNCTION_AS(0x806029F4, MkwPageEndEntrance_Observe_806029f4,
                            "MkwPageEndEntrance_Observe_806029f4");
REGISTER_NATIVE_FUNCTION_AS(0x80601AEC, MkwPageActivate_Observe_80601aec,
                            "MkwPageActivate_Observe_80601aec");
REGISTER_NATIVE_FUNCTION_AS(0x80635080, MkwSectionMgrEnterSection_Observe_80635080,
                            "MkwSectionMgrEnterSection_Observe_80635080");
