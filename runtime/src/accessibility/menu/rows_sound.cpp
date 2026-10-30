#include "accessibility/menu/menu_rows.h"
#include "audio_backend.h"
#include "music_attenuation.h"
#include "runtime_config.h"

// Defined in runtime/src/hle/audio/ax_mix.cpp, whose header is private to the HLE directory;
// the F10 bar declares it the same way.
namespace AxDspHle {
void SetMixWorkerEnabled(bool enabled);
}

// The F10 bar's audio settings plus the mod's kart and roulette knobs. Each row makes the same
// live call plus persist as the bar (settings_overlay.cpp DrawAudioSettings): the config setters
// alone only write the file.

namespace a11y::menu {

std::vector<MenuRow> SoundRows() {
    return {
        Volume("opt_master_volume", [] { return RuntimeConfigFile::AudioVolume(); },
               [](float v) {
                   AudioBackend::Instance().SetMasterVolume(v);
                   RuntimeConfigFile::SetAudioVolume(v);
               }),
        Volume("opt_music_volume", [] { return RuntimeConfigFile::MusicVolume(); },
               [](float v) {
                   MusicAttenuation::SetMusicVolume(v);
                   RuntimeConfigFile::SetMusicVolume(v);
               }),
        Volume("opt_effects_volume", [] { return RuntimeConfigFile::SoundEffectsVolume(); },
               [](float v) {
                   MusicAttenuation::SetSoundEffectsVolume(v);
                   RuntimeConfigFile::SetSoundEffectsVolume(v);
               }),
        Volume("opt_ui_volume", [] { return RuntimeConfigFile::UiVolume(); },
               [](float v) {
                   MusicAttenuation::SetUiVolume(v);
                   RuntimeConfigFile::SetUiVolume(v);
               }),
        Volume("opt_voices_volume", [] { return RuntimeConfigFile::VoicesVolume(); },
               [](float v) {
                   MusicAttenuation::SetVoicesVolume(v);
                   RuntimeConfigFile::SetVoicesVolume(v);
               }),
        Switch("opt_mute", [] { return RuntimeConfigFile::AudioMuted(); },
               [](bool on) {
                   AudioBackend::Instance().SetMuted(on);
                   RuntimeConfigFile::SetAudioMuted(on);
               }),
        // Applied per frame by KartVolume, so persisting the knob is all a change needs. The
        // player's own kart reaches 200%: past 100 the write boosts (the voice clamps the end
        // product).
        Knob("opt_kart_volume", KnobPercentText,
             [] { return RuntimeConfigFile::AccessibilityKartVolume(); },
             [](int v) { RuntimeConfigFile::SetAccessibilityKartVolume(v); }, /*max=*/200),
        Knob("opt_rival_volume", KnobPercentText,
             [] { return RuntimeConfigFile::AccessibilityRivalKartVolume(); },
             [](int v) { RuntimeConfigFile::SetAccessibilityRivalKartVolume(v); }),
        Knob("opt_roulette_volume", KnobPercentText,
             [] { return RuntimeConfigFile::AccessibilityItemRouletteVolume(); },
             [](int v) { RuntimeConfigFile::SetAccessibilityItemRouletteVolume(v); }),
        Switch("opt_external_media", [] { return RuntimeConfigFile::AttenuateMusicWhenMediaPlays(); },
               [](bool on) {
                   MusicAttenuation::SetEnabled(on);
                   RuntimeConfigFile::SetAttenuateMusicWhenMediaPlays(on);
               }),
        // SetMixWorkerEnabled joins any in-flight mix before switching.
        Switch("opt_mix_worker", [] { return RuntimeConfigFile::AudioMixWorkerEnabled(); },
               [](bool on) {
                   AxDspHle::SetMixWorkerEnabled(on);
                   RuntimeConfigFile::SetAudioMixWorker(on);
               }),
    };
}

}  // namespace a11y::menu
