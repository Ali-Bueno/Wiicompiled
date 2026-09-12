#ifndef MKW_ACCESSIBILITY_AUDIO_CUE_VOLUME_H
#define MKW_ACCESSIBILITY_AUDIO_CUE_VOLUME_H

#include <cstddef>

#include "accessibility/audio/cue_service.h"

namespace a11y::audio {

// A cue family with a volume knob of its own, saved as "<configKey> = <percent>" in
// [accessibility]. Hazard has no sound wired to it yet, so it has no knob.
struct CueVolumeKnob {
    CueChannel channel;
    const char* configKey;
};

const CueVolumeKnob* CueVolumeKnobs(size_t& count);

int CueVolumePercent(CueChannel channel);
// Applies to the running service and persists. Percent is clamped to 0..100.
void SetCueVolumePercent(CueChannel channel, int percent);
// Config to service, for start-up and for a reload of the file.
void LoadCueVolumes();

}  // namespace a11y::audio

#endif  // MKW_ACCESSIBILITY_AUDIO_CUE_VOLUME_H
