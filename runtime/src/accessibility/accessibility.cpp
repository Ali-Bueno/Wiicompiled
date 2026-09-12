#include "accessibility.h"

#include "prism_runtime.h"
#include "accessibility/a11y_log.h"
#include "accessibility/audio/cue_service.h"
#include "accessibility/audio/cue_volume.h"
#include "accessibility/config_reload.h"
#include "accessibility/localization.h"
#include "accessibility/menu/settings_menu.h"
#include "accessibility/mii/mii_identity.h"
#include "accessibility/race/race_manager.h"
#include "accessibility/update_check.h"
#include "screen_reader.h"
#include "ui/screen_watcher.h"

namespace a11y {
namespace {

bool g_initialised = false;
bool g_announced = false;

// These entry points are called from translated guest code, which has no handlers and no way to
// unwind - a guest read that faults or an allocation that fails would take the game down with the
// mod. Nothing is allowed to escape. Logged once per entry point: a fault that repeats every frame
// would otherwise fill the log with the same line.
void ReportFault(const char* where, bool& logged) {
    if (logged) {
        return;
    }
    logged = true;
    RT_LOGF(RT_TAG_A11Y, "accessibility: exception escaped %s; suppressed for the rest of the run\n",
            where);
}

void InitImpl() {
    // Phrases first: everything spoken after this point goes through the table.
    loc::Init();
    ScreenReader::Instance().Initialise();
    // Cues stay independent of the reader on purpose: they must still work with no screen reader
    // running, and speech must still work with no audio device.
    audio::CueService::Instance().Start();
    audio::LoadCueVolumes();
    // Before the game builds its licence from the default Mii table.
    mii::Init();
    // Asks the network in the background; nothing here waits for it.
    update::Start();
    RT_LOGF(RT_TAG_A11Y, "accessibility initialised (reader: %s, cues: %s)\n",
            ScreenReader::Instance().BackendName(),
            audio::CueService::Instance().Available() ? "on" : "off");
}

void TickImpl() {
    // Announced from the first presented frame rather than from Init(), so hearing it also
    // confirms the per-frame hook is live.
    if (!g_announced) {
        g_announced = true;
        RT_LOGF(RT_TAG_A11Y, "first frame presented; per-frame hook is live\n");
        ScreenReader::Instance().Speak(loc::Get("ready"));
    }

    // Speaks what the background check found, then asks whether to update now.
    update::Tick();

    // A saved Config.toml applies its [accessibility] edits live - the file is the settings UI
    // until the self-voicing menu exists.
    ConfigReloadTick();

    // Keeps our Mii in the Mii database and the licence on it once the save is loaded.
    mii::Tick();

    // Everything the menus say is decided here: the watcher reads what is on screen and speaks
    // whatever changed since last frame.
    ui::TickScreenWatcher();

    // Reads the kart and the course once, then drives the driving assists from it.
    race::Tick();

    // Drains the actions the host event thread queued; runs here so speech and guest reads
    // stay on the guest thread.
    menu::SettingsMenu::Instance().Tick();

    // Last, so every cue raised this frame is rendered in the same tick that asked for it.
    audio::CueService::Instance().Tick();
}

void ShutdownImpl() {
    race::Reset();
    audio::CueService::Instance().Shutdown();
    ScreenReader::Instance().Shutdown();
    UnloadPrism();
}

}  // namespace

void Init() {
    if (g_initialised) {
        return;
    }
    g_initialised = true;
    try {
        InitImpl();
    } catch (...) {
        static bool logged = false;
        ReportFault("Init", logged);
    }
}

void Tick() {
    if (!g_initialised) {
        return;
    }
    try {
        TickImpl();
    } catch (...) {
        static bool logged = false;
        ReportFault("Tick", logged);
    }
}

void Shutdown() {
    if (!g_initialised) {
        return;
    }
    try {
        ShutdownImpl();
    } catch (...) {
        static bool logged = false;
        ReportFault("Shutdown", logged);
    }
    g_initialised = false;
    g_announced = false;
}

}  // namespace a11y
