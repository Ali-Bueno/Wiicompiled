#include <SDL3/SDL_scancode.h>
#include <dolphin/pad.h>

#include <algorithm>
#include <array>
#include <string>
#include <utility>

#include "accessibility/localization.h"
#include "accessibility/menu/menu_rows.h"
#include "accessibility/menu/rows_controls.h"
#include "accessibility/screen_reader.h"
#include "controller_button_names.h"
#include "input_bindings.h"
#include "runtime_config.h"

// Defined in runtime/src/hle/input/pad.cpp; the F10 bar declares it the same way.
extern "C" void PAD_HLE_SetRumbleEnabled(bool enabled);

// The F10 bar's Controller menu (settings_overlay.cpp DrawControllerSettings and
// DrawKeyboardSettings): same PAD calls, same persistence. The bar re-applies the configured
// buttons by itself every frame once a port's assigned device changes, so assign and unassign
// need nothing beyond the PAD call.

namespace a11y::menu {
namespace {

uint32_t g_port = 0;
uint32_t g_candidate = 0;  // connected controller the assign row is showing

constexpr int kMaxPort = PAD_MAX_CONTROLLERS - 1;

// Same keys as the bar's WASD preset (settings_overlay.cpp), indexed like kGameCubeButtons
// and the PAD axes.
constexpr std::array<int, PAD_BUTTON_COUNT> kWasdButtonKeys = {
    PAD_KEY_MOUSE_LEFT, SDL_SCANCODE_SPACE, SDL_SCANCODE_E, SDL_SCANCODE_Q,
    SDL_SCANCODE_RETURN, PAD_KEY_MOUSE_MIDDLE, SDL_SCANCODE_LSHIFT, PAD_KEY_MOUSE_RIGHT,
    SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
};
constexpr std::array<int, PAD_AXIS_COUNT> kWasdAxisKeys = {
    SDL_SCANCODE_D, SDL_SCANCODE_A, SDL_SCANCODE_W, SDL_SCANCODE_S, SDL_SCANCODE_L,
    SDL_SCANCODE_J, SDL_SCANCODE_I, SDL_SCANCODE_K, SDL_SCANCODE_LSHIFT, PAD_KEY_MOUSE_RIGHT,
};
constexpr int kAxisBindingScale = 1;  // third field of the bar's PADKeyAxisBinding

bool KeyboardActive() {
    uint32_t count = 0;
    return PADGetKeyButtonBindings(g_port, &count) != nullptr;
}

void ApplyWasdPreset() {
    for (size_t i = 0; i < kWasdButtonKeys.size(); ++i) {
        PADSetKeyButtonBinding(g_port, {kWasdButtonKeys[i], ControllerNames::kGameCubeButtons[i].padButton});
    }
    uint32_t axisCount = 0;
    auto* mappings = PADGetKeyAxisBindings(g_port, &axisCount);
    for (uint32_t i = 0; i < axisCount && i < kWasdAxisKeys.size(); ++i) {
        PADSetKeyAxisBinding(g_port, {kWasdAxisKeys[i], mappings[i].padAxis, kAxisBindingScale});
    }
    PADSerializeMappings();
}

void SetKeyboardActive(bool on) {
    PADSetKeyboardActive(g_port, on);
    PADSerializeMappings();
    if (!on) {
        return;
    }
    // First time on: nothing is bound yet, so start from the WASD layout like the bar does.
    uint32_t count = 0;
    const auto* buttons = PADGetKeyButtonBindings(g_port, &count);
    if (buttons != nullptr && std::all_of(buttons, buttons + count, [](const auto& binding) {
            return binding.scancode == PAD_KEY_INVALID;
        })) {
        ApplyWasdPreset();
    }
}

std::string DeviceName(const char* name) {
    return name != nullptr ? std::string(name) : loc::Get("controls_device_none");
}

MenuRow PortRow() {
    MenuRow row;
    row.nameKey = "opt_port";
    row.value = [] {
        return loc::Format("controls_port_value",
                           {{"n", std::to_string(g_port + 1)}, {"device", DeviceName(PADGetName(g_port))}});
    };
    row.adjust = [](int dir) {
        g_port = static_cast<uint32_t>(std::clamp(static_cast<int>(g_port) + dir, 0, kMaxPort));
    };
    return row;
}

MenuRow AssignRow() {
    MenuRow row;
    row.nameKey = "opt_assign_controller";
    row.value = [] {
        const uint32_t count = PADCount();
        if (count == 0) {
            return loc::Get("controls_no_connected");
        }
        g_candidate = std::min(g_candidate, count - 1);
        std::string text = DeviceName(PADGetNameForControllerIndex(g_candidate));
        if (PADGetIndexForPort(g_port) == static_cast<int32_t>(g_candidate)) {
            text += ", " + loc::Get("controls_assigned_here");
        }
        return text;
    };
    row.adjust = [](int dir) {
        const uint32_t count = PADCount();
        if (count > 0) {
            g_candidate = static_cast<uint32_t>(
                std::clamp(static_cast<int>(g_candidate) + dir, 0, static_cast<int>(count) - 1));
        }
    };
    // The menu re-speaks the value afterwards, which now ends in "assigned".
    row.activate = [] {
        if (g_candidate < PADCount()) {
            PADSetPortForIndex(g_candidate, g_port);
        }
    };
    return row;
}

void StopAllMotors() {
    // The game will not send another motor command until its own state machine decides to.
    constexpr std::array<uint32_t, PAD_MAX_CONTROLLERS> stopAll{
        PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD,
    };
    PADControlAllMotors(stopAll.data());
}

void ImportFromDolphin() {
    std::string summary;
    std::string error;
    const int imported = InputBindings::ImportDolphinConfig(InputBindings::DefaultDolphinConfigPath(),
                                                           static_cast<int>(g_port) + 1, g_port, summary, error);
    SayNow(imported < 0 ? loc::Format("controls_import_failed", {{"error", error}}) : summary);
}

}  // namespace

uint32_t ControllerPort() {
    return g_port;
}

void SayNow(const std::string& text) {
    ScreenReader::Instance().Speak(text, /*interrupt=*/true);
}

std::vector<MenuRow> ControlsRows() {
    std::vector<MenuRow> rows = {
        PortRow(),
        AssignRow(),
        Action("opt_unassign", [] {
            PADClearPort(g_port);
            SayNow(loc::Format("controls_unassigned", {{"n", std::to_string(g_port + 1)}}));
        }),
    };
    for (MenuRow& row : PresetRows()) {
        rows.push_back(std::move(row));
    }
    rows.push_back(Switch("opt_vibration", [] { return RuntimeConfigFile::RumbleEnabled(true); },
                          [](bool on) {
                              PAD_HLE_SetRumbleEnabled(on);
                              RuntimeConfigFile::SetRumbleEnabled(on);
                              if (!on) {
                                  StopAllMotors();
                              }
                          }));
    rows.push_back(Switch("opt_keyboard_mouse", KeyboardActive, SetKeyboardActive));
    rows.push_back(Action("opt_wasd_preset", [] {
        if (!KeyboardActive()) {
            SayNow(loc::Get("controls_need_keyboard"));
            return;
        }
        ApplyWasdPreset();
        SayNow(loc::Get("controls_wasd_applied"));
    }));
    rows.push_back(Submenu("menu_wii", WiiRemoteRows()));
    rows.push_back(Action("opt_dolphin_import", ImportFromDolphin));
    return rows;
}

}  // namespace a11y::menu
