#include "accessibility/config_reload.h"

#include <filesystem>
#include <fstream>

#include "accessibility/localization.h"
#include "accessibility/race/race_manager.h"
#include "accessibility/screen_reader.h"
#include "runtime_config.h"

namespace a11y {
namespace {

// Checked about every two seconds: one stat() call on a cached path, so the cost is nil and a
// saved file still applies fast enough to feel live while tuning by ear.
constexpr int kReloadCheckFrames = 120;

int g_reloadFrameCounter = 0;
std::filesystem::file_time_type g_reloadLastWrite{};
bool g_reloadBaselined = false;

}  // namespace

void ConfigReloadTick() {
    if (++g_reloadFrameCounter < kReloadCheckFrames) {
        return;
    }
    g_reloadFrameCounter = 0;

    std::error_code ec;
    // Resolved once: the path never moves, and resolving it walks the shell's known-folder API.
    static const auto path = RuntimeConfigFile::ResolveConfigPath();
    const auto stamp = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return;  // mid-save or missing; the next check will see it
    }
    if (!g_reloadBaselined) {
        // The first check only records the launch-time stamp, so the load at startup is not
        // announced as a reload.
        g_reloadBaselined = true;
        g_reloadLastWrite = stamp;
        return;
    }
    if (stamp == g_reloadLastWrite) {
        return;
    }
    g_reloadLastWrite = stamp;

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return;
    }
    const RuntimeUserConfig fresh = RuntimeConfigFile::ParseConfig(file, path.string());
    RuntimeUserConfig& live = RuntimeConfigFile::Mutable();

    // Only keys PRESENT in the file are applied. An editor's truncate-then-write leaves a moment
    // where the file is empty or half-written; parsing that yields disengaged optionals, and
    // copying them wholesale would silently reset every knob to its default while announcing
    // success. The deliberate trade-off: deleting a key no longer reverts it until relaunch.
    bool changed = false;
    const auto apply = [&changed](auto& target, const auto& source) {
        if (source.has_value() && source != target) {
            target = source;
            changed = true;
        }
    };
    const bool lineWasItem = RuntimeConfigFile::AccessibilityLineFromItemRoute();
    apply(live.accessibilityInvertSteeringPan, fresh.accessibilityInvertSteeringPan);
    apply(live.accessibilitySteeringStrength, fresh.accessibilitySteeringStrength);
    apply(live.accessibilitySteeringSensitivity, fresh.accessibilitySteeringSensitivity);
    apply(live.accessibilitySteeringLookAhead, fresh.accessibilitySteeringLookAhead);
    apply(live.accessibilityEdgeCues, fresh.accessibilityEdgeCues);
    apply(live.accessibilityLineSource, fresh.accessibilityLineSource);
    if (!changed) {
        return;  // spoken only when something audible really changed
    }
    if (RuntimeConfigFile::AccessibilityLineFromItemRoute() != lineWasItem) {
        // A different backbone means different geometry: rebuild the course map in place so the
        // A/B comparison the setting exists for happens mid-race, by ear.
        race::InvalidateCourseMap();
    }

    ScreenReader::Instance().Speak(loc::Get("settings_reloaded"), /*interrupt=*/true);
}

}  // namespace a11y
