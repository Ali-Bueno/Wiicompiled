#ifndef MKW_ACCESSIBILITY_MENU_MENU_ROWS_H
#define MKW_ACCESSIBILITY_MENU_MENU_ROWS_H

#include <functional>
#include <string>
#include <vector>

#include "accessibility/menu/menu_tree.h"

// Shared row shapes for the settings menu tables, and one builder per submenu.

namespace a11y::menu {

// One click of left/right. Fine enough to tune by ear, coarse enough that sweeping the whole
// range is a handful of presses.
constexpr int kStepPercent = 5;

int ToPercent(float value);
// Snaps to the step grid first so a value written by hand in Config.toml still moves cleanly.
float StepVolume(float value, int direction);
int StepKnob(int value, int direction, int max = 100);

std::string PercentText(float value);
std::string KnobPercentText(int value);
std::string DegreesText(int value);
std::string OnOffText(bool value);

MenuRow Submenu(std::string nameKey, std::vector<MenuRow> children);
// Toggles from Activate as well as from left/right.
MenuRow Switch(std::string nameKey, std::function<bool()> get, std::function<void(bool)> set);
// A 0..1 volume in percent steps; set applies it live and persists it.
MenuRow Volume(std::string nameKey, std::function<float()> get, std::function<void(float)> set);
// An integer knob in percent steps up to max, spoken through text.
MenuRow Knob(std::string nameKey, std::function<std::string(int)> text, std::function<int()> get,
             std::function<void(int)> set, int max = 100);
// One of a fixed list of spoken choices, stepped with left/right without wrapping.
MenuRow Choice(std::string nameKey, std::vector<std::string> choiceKeys, std::function<int()> get,
               std::function<void(int)> set);
MenuRow Action(std::string nameKey, std::function<void()> run);

std::vector<MenuRow> SteeringRows();
std::vector<MenuRow> CueRows();
std::vector<MenuRow> SoundRows();
std::vector<MenuRow> GraphicsRows();
std::vector<MenuRow> ControlsRows();

}  // namespace a11y::menu

#endif  // MKW_ACCESSIBILITY_MENU_MENU_ROWS_H
