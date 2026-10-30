#include "accessibility/audio/cue_volume.h"
#include "accessibility/localization.h"
#include "accessibility/menu/menu_rows.h"
#include "accessibility/menu/settings_menu.h"
#include "accessibility/race/drive_assist.h"
#include "accessibility/race/item_beacon.h"
#include "accessibility/race/track_limits.h"
#include "accessibility/race/trick_beacon.h"
#include "runtime_config.h"

// The top of the settings menu and its accessibility submenus. The menu's mechanics (open,
// close, keys, speech) are in settings_menu.cpp; the port's own settings have a file each.

namespace a11y::menu {
namespace {

// The same previews the demo rows play, so a knob is heard at its new level as it moves.
void PlayCueDemo(audio::CueChannel channel) {
    switch (channel) {
        case audio::CueChannel::Edge:
            race::PlayEdgeCueDemo();
            break;
        case audio::CueChannel::Surface:
            race::PlaySurfaceCueDemo();
            break;
        case audio::CueChannel::Curve:
            race::PlayCurveCueDemo();
            break;
        case audio::CueChannel::Countdown:
            race::PlayCountdownCueDemo();
            break;
        case audio::CueChannel::ItemBox:
            race::PlayItemBoxCueDemo();
            break;
        case audio::CueChannel::TrickRamp:
            race::PlayTrickRampCueDemo();
            break;
        default:
            break;
    }
}

}  // namespace

std::vector<MenuRow> SteeringRows() {
    // Read by the guide on every frame, so persisting is all a change needs. The setters' own
    // clamps hold the floors, so stepping below one just stays there.
    return {
        Switch("opt_invert_pan", [] { return RuntimeConfigFile::AccessibilityInvertSteeringPan(); },
               [](bool on) { RuntimeConfigFile::SetAccessibilityInvertSteeringPan(on); }),
        Knob("opt_steering_strength", KnobPercentText,
             [] { return RuntimeConfigFile::AccessibilitySteeringStrength(); },
             [](int v) { RuntimeConfigFile::SetAccessibilitySteeringStrength(v); },
             RuntimeConfigFile::kSteeringStrengthMax),
        Knob("opt_look_ahead", KnobPercentText,
             [] { return RuntimeConfigFile::AccessibilitySteeringLookAhead(); },
             [](int v) { RuntimeConfigFile::SetAccessibilitySteeringLookAhead(v); },
             RuntimeConfigFile::kSteeringLookAheadMax),
        Knob("opt_steering_lean_angle", DegreesText,
             [] { return RuntimeConfigFile::AccessibilitySteeringLeanAngle(); },
             [](int v) { RuntimeConfigFile::SetAccessibilitySteeringLeanAngle(v); },
             RuntimeConfigFile::kSteeringLeanAngleMax),
    };
}

std::vector<MenuRow> CueRows() {
    std::vector<MenuRow> rows;
    rows.push_back(Switch("opt_edge_cues", [] { return RuntimeConfigFile::AccessibilityEdgeCues(); },
                          [](bool on) { RuntimeConfigFile::SetAccessibilityEdgeCues(on); }));

    // One knob per cue family, applied live and saved; each step replays the cue so it is
    // tuned by ear at the new level.
    size_t knobCount = 0;
    const audio::CueVolumeKnob* knobs = audio::CueVolumeKnobs(knobCount);
    for (size_t i = 0; i < knobCount; ++i) {
        const audio::CueChannel channel = knobs[i].channel;
        rows.push_back(Knob(std::string("opt_") + knobs[i].configKey, KnobPercentText,
                            [channel] { return audio::CueVolumePercent(channel); },
                            [channel](int v) {
                                audio::SetCueVolumePercent(channel, v);
                                PlayCueDemo(channel);
                            },
                            /*max=*/100));
    }

    rows.push_back(Action("demo_edge", [] { race::PlayEdgeCueDemo(); }));
    rows.push_back(Action("demo_curve", [] { race::PlayCurveCueDemo(); }));
    rows.push_back(Action("demo_itembox", [] { race::PlayItemBoxCueDemo(); }));
    rows.push_back(Action("demo_trickramp", [] { race::PlayTrickRampCueDemo(); }));
    return rows;
}

void SettingsMenu::BuildOptions() {
    if (mBuilt) {
        return;
    }
    mBuilt = true;

    // First: the one row a fresh install may still need, and the only one typed rather than
    // stepped. Saved to [system] mii_name and stamped on the Mii at once (docs/mii-name.md).
    mRows.push_back({"opt_mii_name",
                     [] {
                         const std::string name = RuntimeConfigFile::MiiName();
                         return name.empty() ? loc::Get("name_unset") : name;
                     },
                     nullptr,
                     [this] { BeginNameEdit(); },
                     {}});
    mRows.push_back(Submenu("menu_steering", SteeringRows()));
    mRows.push_back(Submenu("menu_cues", CueRows()));
    mRows.push_back(Submenu("menu_sound", SoundRows()));
    mRows.push_back(Submenu("menu_graphics", GraphicsRows()));
    mRows.push_back(Submenu("menu_controls", ControlsRows()));
}

}  // namespace a11y::menu
