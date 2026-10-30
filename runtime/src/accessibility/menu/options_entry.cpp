#include "accessibility/menu/options_entry.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <vector>

#include "abi_bridge.h"
#include "accessibility/a11y_log.h"
#include "accessibility/guest_text.h"
#include "accessibility/localization.h"
#include "accessibility/menu/settings_menu.h"
#include "accessibility/ui/class_probe.h"
#include "isa/ppc_isa_context.h"
#include "memory.h"

namespace a11y::menu {
namespace {

// Game functions, from projects/mkwii/MAP.txt.
constexpr std::uint32_t kOperatorNew = 0x80229DCC;             // new
constexpr std::uint32_t kOperatorDeleteArray = 0x80229EE0;     // delete_a
constexpr std::uint32_t kPushButtonCtor = 0x805BD3A8;          // PushButton::__ct
constexpr std::uint32_t kPushButtonDtor = 0x805BD4B0;          // PushButton::__dt
constexpr std::uint32_t kPushButtonLoad = 0x805BD518;          // PushButton::Load
constexpr std::uint32_t kControlGroupInit = 0x805C2620;        // ControlGroup::Init(Page)
constexpr std::uint32_t kControlGroupSetControl = 0x805C27DC;  // ControlGroup::SetControl
constexpr std::uint32_t kTextInfoCtor = 0x805CD94C;            // Text::Info::__ct
constexpr std::uint32_t kSetMessage = 0x8063DDB4;              // LayoutUIControl::SetMessage
// Copies the base position into the one drawn and hit-tested (UIControl::PositionFromBasePosition).
constexpr std::uint32_t kPositionFromBase = 0x8063D3CC;
constexpr std::uint32_t kGuestCalls[] = {
    kOperatorNew,    kOperatorDeleteArray, kPushButtonCtor,   kPushButtonDtor,
    kPushButtonLoad, kControlGroupInit,    kControlGroupSetControl,
    kTextInfoCtor,   kSetMessage,          kPositionFromBase,
};

// Pages::Options is page 0xC0: Page::Init (0x80601A60) stores the id at +0x4 before OnInit. Its
// vtable's OnActivate slot is checked exactly, never scanned: the section's page vtables sit back
// to back in StaticR. Retro Rewind's page derives from it and keeps that slot.
constexpr std::uint32_t kPageIdOffset = 0x4;
constexpr std::uint32_t kOptionsPageId = 0xC0;
constexpr std::uint32_t kOptionsOnActivate = 0x805FD8E8;  // Pages::Options::OnActivate

// From Page::InitControlGroup (0x8060245C), ControlGroup::Init and ::SetControl.
constexpr std::uint32_t kPageControlGroupOffset = 0x24;
constexpr std::uint32_t kGroupControlsOffset = 0x0;
constexpr std::uint32_t kGroupDrawOrderOffset = 0x4;
constexpr std::uint32_t kGroupOwnerOffset = 0xC;
constexpr std::uint32_t kGroupCountOffset = 0x10;
constexpr std::uint32_t kControlZIndexOffset = 0x84;
// The layout position ControlLoader::LoadVariant writes.
constexpr std::uint32_t kControlXOffset = 0x4;
constexpr std::uint32_t kControlYOffset = 0x8;
// Far above any page's control count; a larger value is not a live group.
constexpr std::uint32_t kMaxControls = 64;

// Options::OnInit lays its PushButtons 0x254 apart. Text::Info::__ct clears 0xC1 bytes and its
// string slots start at +0x9C.
constexpr std::uint32_t kPushButtonSize = 0x254;
constexpr std::uint32_t kTextInfoSize = 0xC4;
constexpr std::uint32_t kTextInfoStringsOffset = 0x9C;
// Menu.bmg 6602 is nothing but string slot 0, in every language of Base and Retro Rewind.
constexpr std::uint32_t kStringSlotMessage = 6602;

// One guest block per page: the button, its Text::Info, the layout names Load reads, the label.
constexpr std::uint32_t kInfoOffset = kPushButtonSize;
constexpr std::uint32_t kNamesOffset = kInfoOffset + kTextInfoSize;
constexpr std::uint32_t kNamesSize = 0x40;
constexpr std::uint32_t kLabelOffset = kNamesOffset + kNamesSize;
constexpr std::uint32_t kLabelUnits = 32;
constexpr std::uint32_t kBlockSize = kLabelOffset + (kLabelUnits + 1) * sizeof(char16_t);

// A vanilla Options button (Channel.szs button/ctrl/OptionTopButton.brctr), which Retro Rewind
// still ships; its position is replaced below the page's own column.
constexpr const char* kLayoutDir = "button";
constexpr const char* kLayoutCtrl = "OptionTopButton";
constexpr const char* kLayoutVariant = "ChannelInstall";
// As the page's own buttons in Options::OnInit: player 1's cursor, not pointer-only.
constexpr std::uint32_t kLoadPlayerFlags = 1;
constexpr std::uint32_t kDtorDeletes = 1;
// Layout units; the game's own positions are whole numbers.
constexpr float kSameColumn = 1.0f;

std::unordered_map<std::uint32_t, std::uint32_t> g_buttons;  // page -> our button

// A nested guest call is made at a call boundary, so the caller's volatile registers are dead;
// the rest is put back however the calls end.
class GuestCalls {
public:
    explicit GuestCalls(CpuContext* ctx) : mCtx(ctx), mSaved(*ctx) {}
    ~GuestCalls() { *mCtx = mSaved; }

    std::uint32_t Call(std::uint32_t address, std::initializer_list<std::uint32_t> args) {
        std::uint32_t reg = 3;
        for (const std::uint32_t arg : args) {
            mCtx->gpr[reg++] = arg;
        }
        InvokeIndirectCpu(address, mCtx);
        return mCtx->gpr[3];
    }

private:
    CpuContext* mCtx;
    const CpuContext mSaved;
};

// A target missing from the dispatch registry is a fatal popup, so it is asked first.
bool TargetsRegistered() {
    return std::all_of(std::begin(kGuestCalls), std::end(kGuestCalls), [](std::uint32_t address) {
        return TranslatedFunctionRegistry::FindByAddressPtr(address) != nullptr;
    });
}

bool IsOptionsPage(std::uint32_t page) {
    std::uint32_t id = 0;
    return Memory::TryRead32(page + kPageIdOffset, id) && id == kOptionsPageId &&
           ui::HasMethodAtSlot(page, ui::kPageOnActivateSlot, kOptionsOnActivate);
}

std::uint32_t PageOfGroup(std::uint32_t group) {
    std::uint32_t page = 0;
    return Memory::TryRead32(group + kGroupOwnerOffset, page) && page != 0 &&
                   group == page + kPageControlGroupOffset
               ? page
               : 0;
}

void WriteName(std::uint32_t address, const char* name) {
    for (std::size_t k = 0, n = std::strlen(name); k <= n; ++k) {
        Memory::Write8(address + static_cast<std::uint32_t>(k), static_cast<std::uint8_t>(name[k]));
    }
}

struct Column {
    std::size_t lowest = 0;  // index of the lowest control of the page's main column
    float x = 0.0f;
    float nextY = 0.0f;      // one gap below it
};

// The column most controls share; the button goes one gap under its lowest member, so the
// cursor reaches it from the last button and the layout needs no position per product.
bool FindColumn(const std::vector<std::uint32_t>& controls, Column& out) {
    std::vector<float> xs;
    std::vector<float> ys;
    for (const std::uint32_t control : controls) {
        xs.push_back(Memory::ReadFloat32(control + kControlXOffset));
        ys.push_back(Memory::ReadFloat32(control + kControlYOffset));
    }
    std::size_t bestCount = 0;
    for (const float x : xs) {
        const auto count = static_cast<std::size_t>(std::count_if(
            xs.begin(), xs.end(), [x](float other) { return std::fabs(other - x) < kSameColumn; }));
        if (count > bestCount) {
            bestCount = count;
            out.x = x;
        }
    }
    std::vector<std::size_t> members;
    for (std::size_t i = 0; i < xs.size(); ++i) {
        if (std::fabs(xs[i] - out.x) < kSameColumn) {
            members.push_back(i);
        }
    }
    if (members.size() < 2) {
        return false;
    }
    std::sort(members.begin(), members.end(),
              [&ys](std::size_t a, std::size_t b) { return ys[a] < ys[b]; });
    out.lowest = members[0];
    out.nextY = ys[members[0]] - (ys[members[1]] - ys[members[0]]);
    return true;
}

void AddButton(CpuContext* ctx, std::uint32_t page, std::uint32_t group) {
    const std::uint32_t count = Memory::Read32(group + kGroupCountOffset);
    const std::uint32_t controlsArray = Memory::Read32(group + kGroupControlsOffset);
    const std::uint32_t drawOrderArray = Memory::Read32(group + kGroupDrawOrderOffset);
    if (count == 0 || count > kMaxControls || controlsArray == 0 || drawOrderArray == 0) {
        return;
    }
    std::vector<std::uint32_t> controls;
    std::vector<std::uint32_t> zIndices;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t control = Memory::Read32(controlsArray + i * sizeof(std::uint32_t));
        if (control == 0) {
            return;  // InitControls has no null check: a half-built group is left alone
        }
        controls.push_back(control);
        zIndices.push_back(Memory::Read32(control + kControlZIndexOffset));
    }
    Column column;
    if (!FindColumn(controls, column)) {
        RT_LOGF(RT_TAG_A11Y, "options entry: page %08x has no button column\n", page);
        return;
    }

    GuestCalls guest(ctx);
    const std::uint32_t button = guest.Call(kOperatorNew, {kBlockSize});
    if (button == 0) {
        return;
    }
    const std::uint32_t dir = button + kNamesOffset;
    const std::uint32_t ctrl = dir + static_cast<std::uint32_t>(std::strlen(kLayoutDir)) + 1;
    const std::uint32_t variant = ctrl + static_cast<std::uint32_t>(std::strlen(kLayoutCtrl)) + 1;
    WriteName(dir, kLayoutDir);
    WriteName(ctrl, kLayoutCtrl);
    WriteName(variant, kLayoutVariant);

    guest.Call(kPushButtonCtor, {button});
    // A group's size is fixed at Init, so it is rebuilt one larger with the same controls.
    guest.Call(kControlGroupInit, {group, page, count + 1});
    guest.Call(kOperatorDeleteArray, {controlsArray});
    guest.Call(kOperatorDeleteArray, {drawOrderArray});
    for (std::uint32_t i = 0; i < count; ++i) {
        guest.Call(kControlGroupSetControl, {group, i, controls[i], zIndices[i]});
    }
    guest.Call(kControlGroupSetControl, {group, count, button, zIndices[column.lowest]});
    guest.Call(kPushButtonLoad, {button, dir, ctrl, variant, kLoadPlayerFlags, 0, 0});

    Memory::WriteFloat32(button + kControlXOffset, column.x);
    Memory::WriteFloat32(button + kControlYOffset, column.nextY);
    guest.Call(kPositionFromBase, {button});
    g_buttons[page] = button;
    RT_LOGF(RT_TAG_A11Y, "options entry: page %08x, %u controls, button %08x at (%.0f, %.0f)\n",
            page, count, button, column.x, column.nextY);
}

void SetLabel(CpuContext* ctx, std::uint32_t button) {
    const std::uint32_t info = button + kInfoOffset;
    const std::uint32_t label = button + kLabelOffset;
    std::u16string text = Utf8ToUtf16(loc::Get("menu_game_button"));
    text.resize(std::min<std::size_t>(text.size(), kLabelUnits));
    for (std::size_t k = 0; k < text.size(); ++k) {
        Memory::Write16(label + static_cast<std::uint32_t>(k * sizeof(char16_t)),
                        static_cast<std::uint16_t>(text[k]));
    }
    Memory::Write16(label + static_cast<std::uint32_t>(text.size() * sizeof(char16_t)), 0);

    GuestCalls guest(ctx);
    guest.Call(kTextInfoCtor, {info});
    Memory::Write32(info + kTextInfoStringsOffset, label);
    guest.Call(kSetMessage, {button, kStringSlotMessage, info});
}

}  // namespace

void OnControlsInitStarting(CpuContext* ctx, std::uint32_t group) noexcept try {
    const std::uint32_t page = PageOfGroup(group);
    if (page == 0 || g_buttons.count(page) != 0 || !IsOptionsPage(page)) {
        return;
    }
    if (!TargetsRegistered()) {
        RT_LOGF(RT_TAG_A11Y, "options entry: skipped, game functions not registered\n");
        return;
    }
    AddButton(ctx, page, group);
} catch (...) {
    RT_LOGF(RT_TAG_A11Y, "options entry: adding the button failed\n");
}

void OnControlsInitDone(CpuContext* ctx, std::uint32_t group) noexcept try {
    const auto it = g_buttons.find(PageOfGroup(group));
    if (it != g_buttons.end()) {
        SetLabel(ctx, it->second);
    }
} catch (...) {
    RT_LOGF(RT_TAG_A11Y, "options entry: labelling the button failed\n");
}

void OnButtonClicked(std::uint32_t button) noexcept {
    for (const auto& [page, ours] : g_buttons) {
        if (ours == button) {
            SettingsMenu::Instance().Enqueue(MenuAction::Open);
            return;
        }
    }
}

void OnOptionsDestroying(CpuContext* ctx, std::uint32_t page) noexcept try {
    const auto it = g_buttons.find(page);
    if (it == g_buttons.end()) {
        return;
    }
    const std::uint32_t button = it->second;
    g_buttons.erase(it);
    GuestCalls guest(ctx);
    guest.Call(kPushButtonDtor, {button, kDtorDeletes});
} catch (...) {
    RT_LOGF(RT_TAG_A11Y, "options entry: destroying the button failed\n");
}

}  // namespace a11y::menu
