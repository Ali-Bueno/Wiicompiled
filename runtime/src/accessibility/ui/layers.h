#ifndef MKW_ACCESSIBILITY_UI_LAYERS_H
#define MKW_ACCESSIBILITY_UI_LAYERS_H

#include <cstdint>
#include <vector>

namespace a11y::ui {

// The section's stack of page layers, bottom first, and only the ones that have settled.
//
// The cursor lives on the top layer; a lot of the text belonging to it does not. A page registers
// controls onto another page through its own CreateExternalControl - which is why kart selection
// handles OnExternalButtonSelect rather than OnButtonSelect - so a screen's tooltip and the name of
// the highlighted item routinely sit a layer down. Reading only the top page leaves those mute.
//
// Empty when the top layer is still animating: there is nothing stable to read then, and that is
// the same gate the watcher used to apply to the top page alone.
std::vector<std::uint32_t> ActiveLayerPages() noexcept;

// The SectionMgr singleton, the root of the whole menu system: the layer stack hangs off it, and so
// does the menu's own record of what each player has picked so far. Zero before the game builds it.
//
// Shared rather than re-derived, so its address is written down once.
std::uint32_t SectionManager() noexcept;

}  // namespace a11y::ui

#endif  // MKW_ACCESSIBILITY_UI_LAYERS_H
