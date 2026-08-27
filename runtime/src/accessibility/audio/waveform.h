#ifndef MKW_ACCESSIBILITY_AUDIO_WAVEFORM_H
#define MKW_ACCESSIBILITY_AUDIO_WAVEFORM_H

namespace a11y::audio {

enum class Waveform { Sine, Triangle, Square, Saw };

// One cycle, phase in [0,1). Already trimmed so the four shapes sit at the same perceived
// loudness - a square at the same peak as a sine reads far louder because of its odd harmonics.
float SampleWaveform(Waveform shape, float phase);

}  // namespace a11y::audio

#endif  // MKW_ACCESSIBILITY_AUDIO_WAVEFORM_H
