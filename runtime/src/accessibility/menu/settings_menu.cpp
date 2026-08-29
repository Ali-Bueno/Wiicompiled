#include "accessibility/menu/settings_menu.h"

#include <algorithm>
#include <cmath>

#include "accessibility/localization.h"
#include "accessibility/race/drive_assist.h"
#include "accessibility/race/item_beacon.h"
#include "accessibility/race/race_state.h"
#include "accessibility/race/track_limits.h"
#include "accessibility/screen_reader.h"
#include "audio_backend.h"
#include "dolphin/pad.h"
#include "music_attenuation.h"
#include "runtime_config.h"

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

std::string OnOffText(bool value) {
    return loc::Get(value ? "value_on" : "value_off");
}

void Say(const std::string& text) {
    // Interrupting keeps the reader on the cursor while scrubbing; queued speech lagged a value
    // behind it in the MK64 menu (docs/menu-accessibility.md §2).
    ScreenReader::Instance().Speak(text, /*interrupt=*/true);
}

}  // namespace

SettingsMenu& SettingsMenu::Instance() {
    static SettingsMenu instance;
    return instance;
}

void SettingsMenu::Enqueue(MenuAction action) {
    std::lock_guard<std::mutex> lock(mQueueMutex);
    mQueue.push_back(action);
}

void SettingsMenu::Tick() {
    std::vector<MenuAction> actions;
    {
        std::lock_guard<std::mutex> lock(mQueueMutex);
        actions.swap(mQueue);
    }
    for (MenuAction action : actions) {
        Apply(action);
    }
}

void SettingsMenu::BuildOptions() {
    if (mBuilt) {
        return;
    }
    mBuilt = true;

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

    // The steering knobs already hot-apply through Mutable(): the assist re-reads them per frame.
    mOptions.push_back({"opt_steering_strength",
                        [] {
                            return std::to_string(RuntimeConfigFile::AccessibilitySteeringStrength());
                        },
                        [](int dir) {
                            RuntimeConfigFile::SetAccessibilitySteeringStrength(StepKnob(
                                RuntimeConfigFile::AccessibilitySteeringStrength(), dir));
                        },
                        nullptr});
    mOptions.push_back(
        {"opt_steering_sensitivity",
         [] { return std::to_string(RuntimeConfigFile::AccessibilitySteeringSensitivity()); },
         [](int dir) {
             RuntimeConfigFile::SetAccessibilitySteeringSensitivity(
                 StepKnob(RuntimeConfigFile::AccessibilitySteeringSensitivity(), dir));
         },
         nullptr});
    mOptions.push_back(
        {"opt_look_ahead",
         [] { return std::to_string(RuntimeConfigFile::AccessibilitySteeringLookAhead()); },
         [](int dir) {
             RuntimeConfigFile::SetAccessibilitySteeringLookAhead(
                 StepKnob(RuntimeConfigFile::AccessibilitySteeringLookAhead(), dir));
         },
         nullptr});
    mOptions.push_back(
        {"opt_position_gain",
         [] { return std::to_string(RuntimeConfigFile::AccessibilitySteeringPositionGain()); },
         [](int dir) {
             RuntimeConfigFile::SetAccessibilitySteeringPositionGain(
                 StepKnob(RuntimeConfigFile::AccessibilitySteeringPositionGain(), dir));
         },
         nullptr});
    mOptions.push_back(
        {"opt_curve_accent",
         [] { return std::to_string(RuntimeConfigFile::AccessibilitySteeringCurveAccent()); },
         [](int dir) {
             RuntimeConfigFile::SetAccessibilitySteeringCurveAccent(
                 StepKnob(RuntimeConfigFile::AccessibilitySteeringCurveAccent(), dir));
         },
         nullptr});
    mOptions.push_back(
        {"opt_pan_curve",
         [] { return std::to_string(RuntimeConfigFile::AccessibilitySteeringPanCurve()); },
         [](int dir) {
             RuntimeConfigFile::SetAccessibilitySteeringPanCurve(
                 StepKnob(RuntimeConfigFile::AccessibilitySteeringPanCurve(), dir));
         },
         nullptr});
    mOptions.push_back(
        {"opt_curve_look_ahead",
         [] { return std::to_string(RuntimeConfigFile::AccessibilityCurveLookAhead()); },
         [](int dir) {
             RuntimeConfigFile::SetAccessibilityCurveLookAhead(
                 StepKnob(RuntimeConfigFile::AccessibilityCurveLookAhead(), dir));
         },
         nullptr});

    const auto toggleInvert = [] {
        RuntimeConfigFile::SetAccessibilityInvertSteeringPan(
            !RuntimeConfigFile::AccessibilityInvertSteeringPan());
    };
    mOptions.push_back({"opt_invert_pan",
                        [] { return OnOffText(RuntimeConfigFile::AccessibilityInvertSteeringPan()); },
                        [toggleInvert](int) { toggleInvert(); },
                        toggleInvert});

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

void SettingsMenu::Open() {
    // The menus are its place: mid-race the driving cues own the audio, and the blocked pad
    // would freeze the kart.
    if (race::ReadRaceState().valid) {
        Say(loc::Get("menu_race_blocked"));
        return;
    }
    BuildOptions();
    mOpen = true;
    mFocus = 0;
    PADBlockInput(true);
    Say(loc::Get("menu_opened"));
    // Queued, so the welcome line is heard before the first row.
    SpeakFocused(/*withName=*/true, /*interrupt=*/false);
}

void SettingsMenu::Close() {
    mOpen = false;
    PADBlockInput(false);
    Say(loc::Get("menu_closed"));
}

void SettingsMenu::SpeakFocused(bool withName, bool interrupt) {
    if (mOptions.empty()) {
        return;
    }
    const Option& option = mOptions[static_cast<size_t>(mFocus)];
    std::string text = withName ? loc::Get(option.nameKey) : std::string();
    if (option.value) {
        if (!text.empty()) {
            text += ", ";
        }
        text += option.value();
    }
    if (!text.empty()) {
        ScreenReader::Instance().Speak(text, interrupt);
    }
}

void SettingsMenu::Apply(MenuAction action) {
    if (!mOpen) {
        if (action == MenuAction::Toggle) {
            Open();
        }
        return;
    }
    switch (action) {
        case MenuAction::Toggle:
        case MenuAction::Back:
            Close();
            break;
        case MenuAction::Up:
        case MenuAction::Down: {
            const int count = static_cast<int>(mOptions.size());
            const int delta = action == MenuAction::Up ? -1 : 1;
            mFocus = (mFocus + delta + count) % count;
            SpeakFocused(/*withName=*/true, /*interrupt=*/true);
            break;
        }
        case MenuAction::Left:
        case MenuAction::Right: {
            Option& option = mOptions[static_cast<size_t>(mFocus)];
            if (option.adjust) {
                option.adjust(action == MenuAction::Left ? -1 : 1);
                SpeakFocused(/*withName=*/false, /*interrupt=*/true);
            }
            break;
        }
        case MenuAction::Activate: {
            Option& option = mOptions[static_cast<size_t>(mFocus)];
            if (option.activate) {
                option.activate();
                if (option.value) {
                    SpeakFocused(/*withName=*/false, /*interrupt=*/true);
                }
            }
            break;
        }
    }
}

}  // namespace a11y::menu
