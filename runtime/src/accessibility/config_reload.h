#ifndef MKW_ACCESSIBILITY_CONFIG_RELOAD_H
#define MKW_ACCESSIBILITY_CONFIG_RELOAD_H

namespace a11y {

// Applies edits to Config.toml's [accessibility] section while the game runs.
//
// The runtime normally reads the file once at launch. Accessibility settings also have a
// self-voicing menu, and edits made directly in the file take effect here without a restart. Only
// accessibility keys are re-read; anything else still needs the restart it always needed.
void ConfigReloadTick();

}  // namespace a11y

#endif  // MKW_ACCESSIBILITY_CONFIG_RELOAD_H
