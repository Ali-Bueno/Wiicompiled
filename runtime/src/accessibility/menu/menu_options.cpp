#include <algorithm>
#include <cmath>

#include "accessibility/audio/cue_volume.h"
#include "accessibility/localization.h"
#include "accessibility/menu/settings_menu.h"
#include "accessibility/race/drive_assist.h"
#include "accessibility/race/item_beacon.h"
#include "accessibility/race/track_limits.h"
#include "audio_backend.h"
#include "music_attenuation.h"
#include "runtime_config.h"

// The rows of the settings menu, in the order they are read. The menu's mechanics (open, close,
// keys, speech) are in settings_menu.cpp; this file is only the table.

namespace a11y::menu {
namespace {

// One click of left/right. Fine enough to tune by ear, coarse enough that sweeping the whole
// range is a handful of presses, matching the F10 bar's percent sliders.
constexpr int kStepPercent = 5;

int ToPercent(float value) {
    return static_cast<int>(std::lround(std::clamp(value, 0.0f, 1.0f) * 100.0f));
}

// Snaps to the step grid first so a value written by hand in Config.toml still moves cleanly.
float StepVolume(float value, int direction) {
    const int snapped =
        static_cast<int>(std::lround(static_cast<float>(ToPercent(value)) / kStepPercent)) *
        kStepPercent;
    return static_cast<float>(std::clamp(snapped + direction * kStepPercent, 0, 100)) / 100.0f;
}

int StepKnob(int value, int direction, int max = 100) {
    const int snapped =
        static_cast<int>(std::lround(static_cast<float>(value) / kStepPercent)) * kStepPercent;
    return std::clamp(snapped + direction * kStepPercent, 0, max);
}

std::string PercentText(float value) {
    return loc::Format("percent", {{"n", std::to_string(ToPercent(value))}});
}

std::string KnobPercentText(int value) {
    return loc::Format("percent", {{"n", std::to_string(value)}});
}

std::string DegreesText(int value) {
    return loc::Format("degrees", {{"n", std::to_string(value)}});
}

std::string OnOffText(bool value) {
    return loc::Get(value ? "value_on" : "value_off");
}

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
        default:
            break;
    }
}

}  // namespace

void SettingsMenu::BuildOptions() {
    if (mBuilt) {
        return;
    }
    mBuilt = true;

    // First: the one row a fresh install may still need, and the only one typed rather than
    // stepped. Saved to [system] mii_name and stamped on the Mii at once (docs/mii-name.md).
    mOptions.push_back({"opt_mii_name",
                        [] {
                            const std::string name = RuntimeConfigFile::MiiName();
                            return name.empty() ? loc::Get("name_unset") : name;
                        },
                        nullptr,
                        [this] { BeginNameEdit(); }});

    // Live-apply + persist, the exact pair the F10 sliders do (settings_overlay.cpp:493): the
    // config setters alone only write the file, they never touch the running audio objects.
    mOptions.push_back({"opt_master_volume",
                        [] { return PercentText(RuntimeConfigFile::AudioVolume()); },
                        [](int dir) {
                            const float v = StepVolume(RuntimeConfigFile::AudioVolume(), dir);
                            AudioBackend::Instance().SetMasterVolume(v);
                            RuntimeConfigFile::SetAudioVolume(v);
                        },
                        nullptr});
    mOptions.push_back({"opt_music_volume",
                        [] { return PercentText(RuntimeConfigFile::MusicVolume()); },
                        [](int dir) {
                            const float v = StepVolume(RuntimeConfigFile::MusicVolume(), dir);
                            MusicAttenuation::SetMusicVolume(v);
                            RuntimeConfigFile::SetMusicVolume(v);
                        },
                        nullptr});

    // Applied per frame by KartVolume, so persisting the knob is all a change needs. The player's
    // own kart reaches 200%: past 100 the write boosts (the voice clamps the end product).
    mOptions.push_back({"opt_kart_volume",
                        [] { return KnobPercentText(RuntimeConfigFile::AccessibilityKartVolume()); },
                        [](int dir) {
                            RuntimeConfigFile::SetAccessibilityKartVolume(StepKnob(
                                RuntimeConfigFile::AccessibilityKartVolume(), dir, /*max=*/200));
                        },
                        nullptr});
    mOptions.push_back(
        {"opt_rival_volume",
         [] { return KnobPercentText(RuntimeConfigFile::AccessibilityRivalKartVolume()); },
         [](int dir) {
             RuntimeConfigFile::SetAccessibilityRivalKartVolume(
                 StepKnob(RuntimeConfigFile::AccessibilityRivalKartVolume(), dir));
         },
         nullptr});
    mOptions.push_back(
        {"opt_roulette_volume",
         [] { return KnobPercentText(RuntimeConfigFile::AccessibilityItemRouletteVolume()); },
         [](int dir) {
             RuntimeConfigFile::SetAccessibilityItemRouletteVolume(
                 StepKnob(RuntimeConfigFile::AccessibilityItemRouletteVolume(), dir));
         },
         nullptr});

    // One knob per cue family, applied live and saved; each step replays the cue so it is
    // tuned by ear at the new level.
    size_t knobCount = 0;
    const audio::CueVolumeKnob* knobs = audio::CueVolumeKnobs(knobCount);
    for (size_t i = 0; i < knobCount; ++i) {
        const audio::CueChannel channel = knobs[i].channel;
        mOptions.push_back({std::string("opt_") + knobs[i].configKey,
                            [channel] { return KnobPercentText(audio::CueVolumePercent(channel)); },
                            [channel](int dir) {
                                audio::SetCueVolumePercent(
                                    channel, StepKnob(audio::CueVolumePercent(channel), dir));
                                PlayCueDemo(channel);
                            },
                            nullptr});
    }

    const auto toggleInvert = [] {
        RuntimeConfigFile::SetAccessibilityInvertSteeringPan(
            !RuntimeConfigFile::AccessibilityInvertSteeringPan());
    };
    mOptions.push_back({"opt_invert_pan",
                        [] { return OnOffText(RuntimeConfigFile::AccessibilityInvertSteeringPan()); },
                        [toggleInvert](int) { toggleInvert(); },
                        toggleInvert});

    // The steering guide's knobs, read by the guide on every frame, so persisting is enough.
    mOptions.push_back(
        {"opt_steering_strength",
         [] { return KnobPercentText(RuntimeConfigFile::AccessibilitySteeringStrength()); },
         [](int dir) {
             RuntimeConfigFile::SetAccessibilitySteeringStrength(
                 StepKnob(RuntimeConfigFile::AccessibilitySteeringStrength(), dir,
                          RuntimeConfigFile::kSteeringStrengthMax));
         },
         nullptr});
    mOptions.push_back(
        {"opt_look_ahead",
         [] { return KnobPercentText(RuntimeConfigFile::AccessibilitySteeringLookAhead()); },
         [](int dir) {
             RuntimeConfigFile::SetAccessibilitySteeringLookAhead(
                 StepKnob(RuntimeConfigFile::AccessibilitySteeringLookAhead(), dir,
                          RuntimeConfigFile::kSteeringLookAheadMax));
         },
         nullptr});
    // The setter's own clamp holds the floor, so stepping below it just stays there.
    mOptions.push_back(
        {"opt_steering_lean_angle",
         [] { return DegreesText(RuntimeConfigFile::AccessibilitySteeringLeanAngle()); },
         [](int dir) {
             RuntimeConfigFile::SetAccessibilitySteeringLeanAngle(
                 StepKnob(RuntimeConfigFile::AccessibilitySteeringLeanAngle(), dir,
                          RuntimeConfigFile::kSteeringLeanAngleMax));
         },
         nullptr});

    const auto toggleEdge = [] {
        RuntimeConfigFile::SetAccessibilityEdgeCues(!RuntimeConfigFile::AccessibilityEdgeCues());
    };
    mOptions.push_back({"opt_edge_cues",
                        [] { return OnOffText(RuntimeConfigFile::AccessibilityEdgeCues()); },
                        [toggleEdge](int) { toggleEdge(); },
                        toggleEdge});

    mOptions.push_back({"demo_edge", nullptr, nullptr, [] { race::PlayEdgeCueDemo(); }});
    mOptions.push_back({"demo_curve", nullptr, nullptr, [] { race::PlayCurveCueDemo(); }});
    mOptions.push_back({"demo_itembox", nullptr, nullptr, [] { race::PlayItemBoxCueDemo(); }});
}

}  // namespace a11y::menu
