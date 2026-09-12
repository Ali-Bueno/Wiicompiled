#include "accessibility/audio/cue_volume.h"

#include <algorithm>
#include <iterator>

#include "runtime_config.h"

namespace a11y::audio {
namespace {

// The config key doubles as the menu row's phrase key ("opt_<key>"), so a knob is one line here.
constexpr CueVolumeKnob kKnobs[] = {
    {CueChannel::Edge, "edge_cue_volume"},
    {CueChannel::Surface, "surface_cue_volume"},
    {CueChannel::Curve, "curve_cue_volume"},
    {CueChannel::Countdown, "countdown_cue_volume"},
    {CueChannel::ItemBox, "item_box_cue_volume"},
};

constexpr int kFullPercent = 100;

const char* ConfigKey(CueChannel channel) {
    for (const CueVolumeKnob& knob : kKnobs) {
        if (knob.channel == channel) {
            return knob.configKey;
        }
    }
    return nullptr;
}

float ToGain(int percent) {
    return static_cast<float>(std::clamp(percent, 0, kFullPercent)) /
           static_cast<float>(kFullPercent);
}

}  // namespace

const CueVolumeKnob* CueVolumeKnobs(size_t& count) {
    count = std::size(kKnobs);
    return kKnobs;
}

int CueVolumePercent(CueChannel channel) {
    const char* key = ConfigKey(channel);
    return key ? RuntimeConfigFile::AccessibilityCueVolume(key) : kFullPercent;
}

void SetCueVolumePercent(CueChannel channel, int percent) {
    const char* key = ConfigKey(channel);
    if (!key) {
        return;
    }
    RuntimeConfigFile::SetAccessibilityCueVolume(key, percent);
    CueService::Instance().SetChannelVolume(channel, ToGain(CueVolumePercent(channel)));
}

void LoadCueVolumes() {
    for (const CueVolumeKnob& knob : kKnobs) {
        CueService::Instance().SetChannelVolume(knob.channel, ToGain(CueVolumePercent(knob.channel)));
    }
}

}  // namespace a11y::audio
