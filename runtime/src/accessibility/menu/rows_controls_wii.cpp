#include "accessibility/localization.h"
#include "accessibility/menu/menu_rows.h"
#include "accessibility/menu/rows_controls.h"
#include "runtime_config.h"
#include "wii_remote_input.h"

// The F10 bar's Wii Remotes (Bluetooth) menu and accelerometer calibration
// (settings_overlay.cpp DrawWiiRemoteSettings, DrawWiiRemoteAccelerometer). The calibration
// result message is not read back: no tick reaches these rows, and polling for it would need
// machinery of its own.

namespace a11y::menu {

std::vector<MenuRow> WiiRemoteRows() {
    return {
        // Read at launch only, so the help says to restart.
        Switch("opt_wii_use", [] { return RuntimeConfigFile::WiiRemotesEnabled(true); },
               [](bool on) { RuntimeConfigFile::SetWiiRemotesEnabled(on); }),
        Switch("opt_wii_scan", [] { return RuntimeConfigFile::WiiContinuousScanEnabled(false); },
               [](bool on) { RuntimeConfigFile::SetWiiContinuousScanEnabled(on); }),
        // The driver hint is read at launch, so a rescan with the setting off would still find
        // remotes; refused like the bar's disabled button.
        Action("opt_wii_rescan", [] {
            if (!RuntimeConfigFile::WiiRemotesEnabled(true)) {
                SayNow(loc::Get("controls_wii_off"));
                return;
            }
            WiiRemoteInput::RescanNow();
            SayNow(loc::Get("controls_wii_scanning"));
        }),
        Action("opt_wii_calibrate", [] {
            if (!WiiRemoteInput::IsAccelCalibrating()) {
                WiiRemoteInput::StartAccelCalibration(ControllerPort());
            }
            SayNow(loc::Get("controls_wii_calibrating"));
        }),
        Action("opt_wii_clear_calibration", [] {
            if (WiiRemoteInput::IsAccelCalibrating() || !RuntimeConfigFile::HasWiiAccelOffset()) {
                SayNow(loc::Get("controls_wii_nothing_to_clear"));
                return;
            }
            WiiRemoteInput::ClearAccelCalibration();
            SayNow(loc::Get("controls_wii_cleared"));
        }),
    };
}

}  // namespace a11y::menu
