#include "accessibility/menu/menu_rows.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "accessibility/localization.h"

namespace a11y::menu {

int ToPercent(float value) {
    return static_cast<int>(std::lround(std::clamp(value, 0.0f, 1.0f) * 100.0f));
}

float StepVolume(float value, int direction) {
    return static_cast<float>(StepKnob(ToPercent(value), direction)) / 100.0f;
}

int StepKnob(int value, int direction, int max) {
    const int snapped =
        static_cast<int>(std::lround(static_cast<float>(value) / kStepPercent)) * kStepPercent;
    return std::clamp(snapped + direction * kStepPercent, 0, max);
}

std::string PercentText(float value) {
    return KnobPercentText(ToPercent(value));
}

std::string KnobPercentText(int value) {
    return loc::Format("percent", {{"n", std::to_string(value)}});
}

std::string DegreesText(int value) {
    return loc::Format("degrees", {{"n", std::to_string(value)}});
}

std::string OnOffText(bool value) {
    return loc::Get(value ? "value_on" : "value_off");
}

MenuRow Submenu(std::string nameKey, std::vector<MenuRow> children) {
    MenuRow row;
    row.nameKey = std::move(nameKey);
    row.children = std::move(children);
    return row;
}

MenuRow Switch(std::string nameKey, std::function<bool()> get, std::function<void(bool)> set) {
    const auto toggle = [get, set] { set(!get()); };
    return {std::move(nameKey), [get] { return OnOffText(get()); },
            [toggle](int) { toggle(); }, toggle, {}};
}

MenuRow Volume(std::string nameKey, std::function<float()> get, std::function<void(float)> set) {
    return {std::move(nameKey), [get] { return PercentText(get()); },
            [get, set](int dir) { set(StepVolume(get(), dir)); }, nullptr, {}};
}

MenuRow Knob(std::string nameKey, std::function<std::string(int)> text, std::function<int()> get,
             std::function<void(int)> set, int max) {
    return {std::move(nameKey), [text, get] { return text(get()); },
            [get, set, max](int dir) { set(StepKnob(get(), dir, max)); }, nullptr, {}};
}

MenuRow Choice(std::string nameKey, std::vector<std::string> choiceKeys, std::function<int()> get,
               std::function<void(int)> set) {
    const int last = static_cast<int>(choiceKeys.size()) - 1;
    return {std::move(nameKey),
            [choiceKeys, get] {
                const int index = get();
                return index >= 0 && index < static_cast<int>(choiceKeys.size())
                           ? loc::Get(choiceKeys[static_cast<size_t>(index)])
                           : std::string();
            },
            [get, set, last](int dir) {
                const int next = std::clamp(get() + dir, 0, last);
                if (next != get()) {
                    set(next);
                }
            },
            nullptr,
            {}};
}

MenuRow Action(std::string nameKey, std::function<void()> run) {
    return {std::move(nameKey), nullptr, nullptr, std::move(run), {}};
}

}  // namespace a11y::menu
