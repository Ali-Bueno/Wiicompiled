#include "accessibility.h"

#include "prism_runtime.h"
#include "accessibility/a11y_log.h"
#include "accessibility/audio/cue_service.h"
#include "accessibility/config_reload.h"
#include "accessibility/race/race_manager.h"
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
    // Cues stay independent of the reader on purpose: they must still work with no screen reader
    // running, and speech must still work with no audio device.
    audio::CueService::Instance().Start();
    RT_LOGF(RT_TAG_A11Y, "accessibility initialised (reader: %s, cues: %s)\n",
            ScreenReader::Instance().BackendName(),
            audio::CueService::Instance().Available() ? "on" : "off");
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

    // A saved Config.toml applies its [accessibility] edits live - the file is the settings UI
    // until the self-voicing menu exists.
    ConfigReloadTick();

    // Everything the menus say is decided here: the watcher reads what is on screen and speaks
    // whatever changed since last frame.
    ui::TickScreenWatcher();

    // Reads the kart and the course once, then drives the driving assists from it.
    race::Tick();

    // Last, so every cue raised this frame is rendered in the same tick that asked for it.
    audio::CueService::Instance().Tick();
}

void Shutdown() {
    if (!g_initialised) {
        return;
    }
    race::Reset();
    audio::CueService::Instance().Shutdown();
    ScreenReader::Instance().Shutdown();
    UnloadPrism();
    g_initialised = false;
    g_announced = false;
}

}  // namespace a11y
