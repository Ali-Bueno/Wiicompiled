#ifndef MKW_ACCESSIBILITY_AUDIO_WAVEFORM_H
#define MKW_ACCESSIBILITY_AUDIO_WAVEFORM_H

namespace a11y::audio {

enum class Waveform { Sine, Triangle, Square, Saw };

// One cycle, phase in [0,1). Already trimmed so the four shapes sit at the same perceived
// loudness - a square at the same peak as a sine reads far louder because of its odd harmonics.
float SampleWaveform(Waveform shape, float phase);

// Equal-power pan, so a centred cue does not sit in the ~3 dB hole a linear crossfade leaves.
// pan is -1 hard left to +1 hard right.
void PanGains(float pan, float& left, float& right);

// Unity below the knee, then a smooth curve to full scale. Pass the loudest single voice in the
// block as the knee and a cue playing alone comes out untouched, while summed voices lose their
// excess instead of being square-cut at the output rail.
float SoftLimit(float value, float knee);

}  // namespace a11y::audio

#endif  // MKW_ACCESSIBILITY_AUDIO_WAVEFORM_H
