// Observation hooks for the mod's button on the game's Options page (options_entry.cpp).
// REGISTER_NATIVE_FUNCTION_AS keeps the translated bodies, which run unchanged; the registration
// also turns their static call sites into registry calls, so the wrappers fire (CLAUDE.md §3).

#include <cstdint>

#include "abi_bridge.h"
#include "accessibility/menu/options_entry.h"
#include "isa/ppc_isa_context.h"

// Addresses come from projects/mkwii/MAP.txt; the symbol each one is named after is in the comment.
extern "C" {
void func_805C2868(CpuContext* ctx);  // ControlGroup::InitControls
void func_805BE358(CpuContext* ctx);  // PushButton::HandleClick
void func_805FD638(CpuContext* ctx);  // Pages::Options::__dt
}

extern "C" void MkwControlGroupInitControls_Observe_805c2868(CpuContext* ctx) {
    const std::uint32_t group = ctx->gpr[3];
    a11y::menu::OnControlsInitStarting(ctx, group);
    func_805C2868(ctx);
    a11y::menu::OnControlsInitDone(ctx, group);
}

extern "C" void MkwPushButtonHandleClick_Observe_805be358(CpuContext* ctx) {
    const std::uint32_t button = ctx->gpr[3];
    func_805BE358(ctx);
    a11y::menu::OnButtonClicked(button);
}

extern "C" void MkwOptionsDtor_Observe_805fd638(CpuContext* ctx) {
    a11y::menu::OnOptionsDestroying(ctx, ctx->gpr[3]);
    func_805FD638(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x805C2868, MkwControlGroupInitControls_Observe_805c2868,
                            "MkwControlGroupInitControls_Observe_805c2868");
REGISTER_NATIVE_FUNCTION_AS(0x805BE358, MkwPushButtonHandleClick_Observe_805be358,
                            "MkwPushButtonHandleClick_Observe_805be358");
REGISTER_NATIVE_FUNCTION_AS(0x805FD638, MkwOptionsDtor_Observe_805fd638,
                            "MkwOptionsDtor_Observe_805fd638");
