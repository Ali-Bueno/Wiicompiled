#include "accessibility.h"

#include "prism_runtime.h"
#include "accessibility/a11y_log.h"
#include "screen_reader.h"
#include "ui/screen_watcher.h"

namespace a11y {
namespace {

bool g_initialised = false;
bool g_announced = false;

}  // namespace

void Init() {
    if (g_initialised) {
        return;
    }
    g_initialised = true;

    ScreenReader::Instance().Initialise();
    RT_LOGF(RT_TAG_A11Y, "accessibility initialised (reader: %s)\n",
            ScreenReader::Instance().BackendName());
}

void Tick() {
    if (!g_initialised) {
        return;
    }

    // Announced from the first presented frame rather than from Init(), so hearing it also
    // confirms the per-frame hook is live.
    if (!g_announced) {
        g_announced = true;
        RT_LOGF(RT_TAG_A11Y, "first frame presented; per-frame hook is live\n");
        ScreenReader::Instance().Speak("Mario Kart Wii accessibility ready.");
    }

    // Everything the menus say is decided here: the watcher reads what is on screen and speaks
    // whatever changed since last frame.
    ui::TickScreenWatcher();
}

void Shutdown() {
    if (!g_initialised) {
        return;
    }
    ScreenReader::Instance().Shutdown();
    UnloadPrism();
    g_initialised = false;
    g_announced = false;
}

}  // namespace a11y
