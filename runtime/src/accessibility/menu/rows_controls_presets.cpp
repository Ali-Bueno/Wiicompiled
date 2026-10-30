#include <dolphin/pad.h>

#include <algorithm>
#include <array>
#include <functional>

#include "accessibility/localization.h"
#include "accessibility/menu/menu_rows.h"
#include "accessibility/menu/rows_controls.h"
#include "controller_button_names.h"
#include "runtime_config.h"

// The F10 bar's three button presets (settings_overlay.cpp, "Presets"): same PAD calls, and the
// same Config.toml writes so a restart keeps the layout.

namespace a11y::menu {
namespace {

using Preset = std::array<const char*, PAD_BUTTON_COUNT>;

// Tables copied from settings_overlay.cpp (kClassicProPreset, kPlayStationPreset), indexed
// like ControllerNames::kGameCubeButtons.
constexpr Preset kClassicPro = {
    "east", "south", "north", "west", "start", "back", "left_shoulder", "right_shoulder",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
};
constexpr Preset kPlayStation = {
    "south", "east", "west", "north", "start", "back", "left_shoulder", "right_shoulder",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
};

// Buttons can only be mapped once a device is on the port.
bool PortHasMappings() {
    uint32_t count = 0;
    return PADGetButtonMappings(ControllerPort(), &count) != nullptr && count == PAD_BUTTON_COUNT;
}

void ApplyGameCube() {
    const uint32_t port = ControllerPort();
    PADRestoreDefaultMapping(port);
    uint32_t count = 0;
    if (PADButtonMapping* restored = PADGetButtonMappings(port, &count)) {
        for (size_t i = 0; i < ControllerNames::kGameCubeButtons.size(); ++i) {
            const auto it = std::find_if(restored, restored + count, [i](const PADButtonMapping& mapping) {
                return mapping.padButton == ControllerNames::kGameCubeButtons[i].padButton;
            });
            if (it != restored + count) {
                RuntimeConfigFile::SetControllerButton(
                    i, ControllerNames::NativeButtonForValue(it->nativeButton).configName);
            }
        }
    }
    PADSerializeMappings();
}

void ApplyTable(const Preset& preset) {
    const uint32_t port = ControllerPort();
    for (size_t i = 0; i < ControllerNames::kGameCubeButtons.size(); ++i) {
        const ControllerNames::NativeButtonItem* native = ControllerNames::FindNativeButton(preset[i]);
        if (native == nullptr) {
            continue;
        }
        const PADButton button = ControllerNames::kGameCubeButtons[i].padButton;
        PADSetButtonMapping(port, PADButtonMapping{native->nativeButton, button});
        PADSetAltButtonMapping(port, PADButtonMapping{PAD_NATIVE_BUTTON_INVALID, button});
        RuntimeConfigFile::SetControllerButton(i, preset[i]);
    }
    PADSerializeMappings();
}

MenuRow PresetRow(const char* nameKey, std::function<void()> apply) {
    return Action(nameKey, [apply] {
        if (!PortHasMappings()) {
            SayNow(loc::Get("controls_need_controller"));
            return;
        }
        apply();
        SayNow(loc::Get("controls_preset_applied"));
    });
}

}  // namespace

std::vector<MenuRow> PresetRows() {
    return {
        PresetRow("opt_preset_gamecube", ApplyGameCube),
        PresetRow("opt_preset_classic", [] { ApplyTable(kClassicPro); }),
        PresetRow("opt_preset_playstation", [] { ApplyTable(kPlayStation); }),
    };
}

}  // namespace a11y::menu
