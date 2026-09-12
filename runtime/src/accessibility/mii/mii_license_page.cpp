#include "accessibility/mii/mii_license_page.h"

#include <cstdint>
#include <vector>

#include "abi_bridge.h"
#include "accessibility/a11y_log.h"
#include "accessibility/mii/mii_data.h"
#include "accessibility/ui/class_probe.h"
#include "accessibility/ui/layers.h"
#include "isa/ppc_isa_context.h"
#include "memory.h"

namespace a11y::mii {
namespace {

// Pages::LicenseSelect, recognised by the OnActivate its vtable carries (MAP.txt), never by a
// vtable address.
constexpr std::uint32_t kLicenseSelectOnActivate = 0x805EB774;

// Pages::LicenseSelect::OnInit (0x805EABB8) paints each button in two calls: the page's MiiGroup
// slot is reloaded from the licence's createID, then SetControlMii copies the face and formats
// the name out of that slot. Both are idempotent, so the same pair on the live page is the
// repaint; Pages::LicenseChangeMii::UpdateMiiGroup does the first one live.
constexpr std::uint32_t kMiiGroupLoadStoreMii = 0x805FA6E0;      // (group, slot, createId*)
constexpr std::uint32_t kLicenseSelectSetControlMii = 0x805EAE94;  // (unused, button, index, group, slot)

// Page layout, from the ctor (0x805EA834) and OnInit: MiiGroup member, then the four
// LicenseButtons constructed as an array.
constexpr std::uint32_t kPageMiiGroupOffset = 0x1270;
constexpr std::uint32_t kPageButtonsOffset = 0x468;
constexpr std::uint32_t kButtonStride = 0x254;
constexpr std::uint32_t kButtonCount = 4;

// SetControlMii's own validity test: the licence's RKPD save block carries the magic.
constexpr std::uint32_t kRkpdMagicOffset = 8;
constexpr std::uint32_t kRkpdMagic = 0x524B5044;  // 'RKPD'

bool LicenseValid(std::uint32_t manager, std::uint32_t index) {
    std::uint32_t rkpd = 0;
    std::uint32_t magic = 0;
    return Memory::TryRead32(manager + kRkpdPtrOffset, rkpd) && rkpd != 0 &&
           Memory::TryRead32(rkpd + kRkpdMagicOffset + index * kRkpdStride, magic) &&
           magic == kRkpdMagic;
}

// A target missing from the dispatch registry is a fatal popup, so it is asked first.
bool Registered(std::uint32_t address) {
    return TranslatedFunctionRegistry::FindByAddressPtr(address) != nullptr;
}

}  // namespace

bool RepaintLicenseSelect() {
    const std::vector<std::uint32_t> layers = ui::ActiveLayerPages();
    if (layers.empty() || !ui::ImplementsMethod(layers.back(), kLicenseSelectOnActivate)) {
        return false;
    }
    std::uint32_t manager = 0;
    if (!Memory::TryRead32(kLicenseMgrPtr, manager) || manager == 0) {
        return true;
    }
    if (!Registered(kMiiGroupLoadStoreMii) || !Registered(kLicenseSelectSetControlMii)) {
        RT_LOGF(RT_TAG_A11Y, "mii: licence select repaint skipped, targets not registered\n");
        return true;
    }
    const std::uint32_t page = layers.back();
    const std::uint32_t group = page + kPageMiiGroupOffset;

    // A nested guest call from the frame tick, the way the HLE overrides make them: the game is
    // at a call boundary, so its volatile registers are dead, and the rest is put back after.
    CpuContext* ctx = g_currentCpuContext != nullptr ? g_currentCpuContext
                                                     : &GetPersistentCpuContext();
    const CpuContext saved = *ctx;
    int repainted = 0;
    for (std::uint32_t i = 0; i < kButtonCount; ++i) {
        if (!LicenseValid(manager, i)) {
            continue;
        }
        const std::uint32_t license = manager + kLicensesOffset + i * kLicenseStride;
        ctx->gpr[3] = group;
        ctx->gpr[4] = i;
        ctx->gpr[5] = license + kLicenseCreateIdOffset;
        InvokeIndirectCpu(kMiiGroupLoadStoreMii, ctx);
        ctx->gpr[3] = page;
        ctx->gpr[4] = page + kPageButtonsOffset + i * kButtonStride;
        ctx->gpr[5] = i;
        ctx->gpr[6] = group;
        ctx->gpr[7] = i;
        InvokeIndirectCpu(kLicenseSelectSetControlMii, ctx);
        ++repainted;
    }
    *ctx = saved;
    RT_LOGF(RT_TAG_A11Y, "mii: licence select repainted (%d licences)\n", repainted);
    return true;
}

}  // namespace a11y::mii
