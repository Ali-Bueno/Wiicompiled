#ifndef MKW_ACCESSIBILITY_CONFIG_RELOAD_H
#define MKW_ACCESSIBILITY_CONFIG_RELOAD_H

namespace a11y {

// Applies edits to Config.toml's [accessibility] section while the game runs.
//
// The runtime reads the file exactly once, at launch - which a blind player tuning the steering
// guide discovered the hard way: three edits to `steering_look_ahead` mid-game and "siento que va
// igual", because none of them ever applied. The F10 bar cannot be their editor (ImGui is
// unreadable to a screen reader) and the self-voicing menu does not exist yet, so until it does,
// saving the file IS the settings UI. Only the accessibility keys are re-read; anything else in
// the file still needs the restart it always needed.
void ConfigReloadTick();

}  // namespace a11y

#endif  // MKW_ACCESSIBILITY_CONFIG_RELOAD_H
