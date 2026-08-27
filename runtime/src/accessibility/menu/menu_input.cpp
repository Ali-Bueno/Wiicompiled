#include <SDL3/SDL_events.h>

#include "accessibility/accessibility.h"
#include "accessibility/menu/settings_menu.h"
#include "aurora/event.h"

// Host-side input for the settings menu: L3+R3 (or F8) toggles it, dpad/arrows drive it, A/Enter
// activates, B/Escape closes. L3/R3 are safe to claim — no default mapping sends them to the
// game, and the guest pad contract never carries stick clicks at all. While the menu is open the
// guest pad is blocked (PADBlockInput, from the menu itself), so none of this leaks into the game.

namespace a11y {
namespace {

using menu::MenuAction;
using menu::SettingsMenu;

bool g_leftStickDown = false;
bool g_rightStickDown = false;

void OnGamepadButton(const SDL_GamepadButtonEvent& event, bool down) {
    SettingsMenu& menuInstance = SettingsMenu::Instance();
    switch (event.button) {
        case SDL_GAMEPAD_BUTTON_LEFT_STICK:
            g_leftStickDown = down;
            break;
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
            g_rightStickDown = down;
            break;
        default:
            break;
    }
    if (!down) {
        return;
    }
    // Fires once, on whichever stick click completes the pair.
    if ((event.button == SDL_GAMEPAD_BUTTON_LEFT_STICK ||
         event.button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) &&
        g_leftStickDown && g_rightStickDown) {
        menuInstance.Enqueue(MenuAction::Toggle);
        return;
    }
    if (!menuInstance.IsOpen()) {
        return;
    }
    switch (event.button) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP:
            menuInstance.Enqueue(MenuAction::Up);
            break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
            menuInstance.Enqueue(MenuAction::Down);
            break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
            menuInstance.Enqueue(MenuAction::Left);
            break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
            menuInstance.Enqueue(MenuAction::Right);
            break;
        case SDL_GAMEPAD_BUTTON_SOUTH:
            menuInstance.Enqueue(MenuAction::Activate);
            break;
        case SDL_GAMEPAD_BUTTON_EAST:
            menuInstance.Enqueue(MenuAction::Back);
            break;
        default:
            break;
    }
}

void OnKey(const SDL_KeyboardEvent& event) {
    SettingsMenu& menuInstance = SettingsMenu::Instance();
    if (event.scancode == SDL_SCANCODE_F8 && !event.repeat) {
        menuInstance.Enqueue(MenuAction::Toggle);
        return;
    }
    if (!menuInstance.IsOpen()) {
        return;
    }
    // Repeats are welcome on the arrows: holding right is how a volume gets swept.
    switch (event.scancode) {
        case SDL_SCANCODE_UP:
            menuInstance.Enqueue(MenuAction::Up);
            break;
        case SDL_SCANCODE_DOWN:
            menuInstance.Enqueue(MenuAction::Down);
            break;
        case SDL_SCANCODE_LEFT:
            menuInstance.Enqueue(MenuAction::Left);
            break;
        case SDL_SCANCODE_RIGHT:
            menuInstance.Enqueue(MenuAction::Right);
            break;
        case SDL_SCANCODE_RETURN:
            if (!event.repeat) {
                menuInstance.Enqueue(MenuAction::Activate);
            }
            break;
        case SDL_SCANCODE_ESCAPE:
            if (!event.repeat) {
                menuInstance.Enqueue(MenuAction::Back);
            }
            break;
        default:
            break;
    }
}

}  // namespace

void HandleEvents(const AuroraEvent* events) noexcept {
    for (const AuroraEvent* ev = events; ev->type != AURORA_NONE; ++ev) {
        if (ev->type != AURORA_SDL_EVENT) {
            continue;
        }
        const SDL_Event& sdl = ev->sdl;
        switch (sdl.type) {
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                OnGamepadButton(sdl.gbutton, /*down=*/true);
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                OnGamepadButton(sdl.gbutton, /*down=*/false);
                break;
            case SDL_EVENT_KEY_DOWN:
                OnKey(sdl.key);
                break;
            default:
                break;
        }
    }
}

}  // namespace a11y
