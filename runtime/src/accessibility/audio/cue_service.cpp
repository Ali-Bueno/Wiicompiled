#include "accessibility/audio/cue_service.h"

#include <algorithm>
#include <cmath>

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_init.h>

#include "accessibility/a11y_log.h"

namespace a11y::audio {
namespace {

constexpr int kChannels = 2;

// Three frames of audio kept queued. The tick that refills this is one frame apart, so three
// survives a hitch, and the resulting latency stays under the ~100 ms where a cue starts to feel
// detached from what caused it.
constexpr float kFrameSec = 1.0f / 60.0f;
constexpr int kQueuedFrames = 3;
constexpr int kTargetQueueSamples = static_cast<int>(kSampleRate * kFrameSec * kQueuedFrames);

constexpr float kQuarterPi = 0.785398163397448f;
constexpr float kInt16Peak = 32767.0f;

// Equal-power pan, so a centred cue does not sit in the ~3 dB hole a linear crossfade leaves.
void PanGains(float pan, float& left, float& right) {
    const float angle = (std::clamp(pan, -1.0f, 1.0f) + 1.0f) * kQuarterPi;
    left = std::cos(angle);
    right = std::sin(angle);
}

// Per-sample envelope step for a ramp of the given length. A zero-length ramp steps straight to
// the target rather than dividing by zero.
float StepFor(float seconds) {
    return seconds > 0.0f ? 1.0f / (seconds * static_cast<float>(kSampleRate)) : 1.0f;
}

}  // namespace

CueService& CueService::Instance() {
    static CueService instance;
    return instance;
}

bool CueService::Start() {
    if (mStream) {
        return true;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        RT_LOGF(RT_TAG_A11Y, "cue audio: SDL_InitSubSystem failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16LE;
    spec.channels = kChannels;
    spec.freq = kSampleRate;

    SDL_AudioStream* stream =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream) {
        RT_LOGF(RT_TAG_A11Y, "cue audio: SDL_OpenAudioDeviceStream failed: %s\n", SDL_GetError());
        return false;
    }
    if (!SDL_ResumeAudioStreamDevice(stream)) {
        RT_LOGF(RT_TAG_A11Y, "cue audio: SDL_ResumeAudioStreamDevice failed: %s\n", SDL_GetError());
        SDL_DestroyAudioStream(stream);
        return false;
    }
    SDL_SetAudioStreamGain(stream, mMasterVolume);

    mStream = stream;
    SampleBank::Instance().Load();
    RT_LOGF(RT_TAG_A11Y, "cue audio: stream open at %d Hz\n", kSampleRate);
    return true;
}

void CueService::Shutdown() {
    if (!mStream) {
        return;
    }
    SDL_DestroyAudioStream(mStream);
    mStream = nullptr;
    for (Voice& voice : mVoices) {
        voice = Voice{};
    }
}

void CueService::SetMasterVolume(float volume) {
    mMasterVolume = std::clamp(volume, 0.0f, 1.0f);
    if (mStream) {
        SDL_SetAudioStreamGain(mStream, mMasterVolume);
    }
}

void CueService::Apply(Voice& voice, const CueSpec& spec, bool sustained, bool restart) {
    voice.shape = spec.shape;
    voice.targetFreq = std::max(spec.frequencyHz, 0.0f);
    voice.targetAmp = std::clamp(spec.amplitude, 0.0f, 1.0f);
    PanGains(spec.pan, voice.targetGainL, voice.targetGainR);
    voice.sustained = sustained;
    voice.stopping = false;
    voice.attackStep = StepFor(spec.attackSec);
    voice.releaseStep = StepFor(spec.releaseSec);
    voice.sample = SampleBank::Instance().Get(spec.sample);
    voice.rate = std::max(spec.pitch, 0.0f);
    if (restart) {
        voice.samplePos = 0.0f;
    }
    if (!sustained) {
        voice.remainingSec = spec.durationSec;
    }

    // A silent channel has nothing to glide from, so it starts exactly where it was asked to.
    if (restart || !voice.active) {
        voice.freq = voice.targetFreq;
        voice.amp = voice.targetAmp;
        voice.gainL = voice.targetGainL;
        voice.gainR = voice.targetGainR;
        if (restart) {
            voice.phase = 0.0f;
            voice.env = 0.0f;
        }
        voice.active = true;
    }
}

void CueService::PlayOneShot(CueChannel channel, const CueSpec& spec) {
    if (!mStream) {
        return;
    }
    Apply(VoiceFor(channel), spec, /*sustained=*/false, /*restart=*/true);
}

void CueService::SetSustained(CueChannel channel, const CueSpec& spec) {
    if (!mStream) {
        return;
    }
    Apply(VoiceFor(channel), spec, /*sustained=*/true, /*restart=*/false);
}

void CueService::Stop(CueChannel channel) {
    Voice& voice = VoiceFor(channel);
    if (voice.active) {
        voice.stopping = true;  // glide out; a hard cut clicks
    }
}

void CueService::StopAll() {
    for (int i = 0; i < static_cast<int>(CueChannel::Count); ++i) {
        Stop(static_cast<CueChannel>(i));
    }
}

void CueService::Render(int frames) {
    mMix.assign(static_cast<size_t>(frames) * kChannels, 0.0f);
    const float invFrames = 1.0f / static_cast<float>(frames);
    const float secPerSample = 1.0f / static_cast<float>(kSampleRate);

    for (Voice& voice : mVoices) {
        if (!voice.active) {
            continue;
        }
        // Targets only move between ticks, so ramping across the whole block is exactly the
        // smoothing a re-steered pan needs, and costs nothing beyond the four steps.
        const float freqStep = (voice.targetFreq - voice.freq) * invFrames;
        const float ampStep = (voice.targetAmp - voice.amp) * invFrames;
        const float gainLStep = (voice.targetGainL - voice.gainL) * invFrames;
        const float gainRStep = (voice.targetGainR - voice.gainR) * invFrames;

        for (int i = 0; i < frames; ++i) {
            // A sample plays to its own end; only synthesized one-shots run on the timer.
            if (!voice.sample && !voice.sustained && !voice.stopping) {
                voice.remainingSec -= secPerSample;
                if (voice.remainingSec <= 0.0f) {
                    voice.stopping = true;
                }
            }
            const float target = voice.stopping ? 0.0f : 1.0f;
            const float step = target > voice.env ? voice.attackStep : voice.releaseStep;
            voice.env += std::clamp(target - voice.env, -step, step);

            float raw = 0.0f;
            if (voice.sample) {
                const std::vector<float>& pcm = voice.sample->mono;
                const size_t index = static_cast<size_t>(voice.samplePos);
                if (index + 1 < pcm.size()) {
                    const float frac = voice.samplePos - static_cast<float>(index);
                    raw = pcm[index] + (pcm[index + 1] - pcm[index]) * frac;
                    voice.samplePos += voice.rate;
                } else {
                    voice.stopping = true;
                }
            } else {
                raw = SampleWaveform(voice.shape, voice.phase);
                voice.phase += voice.freq * secPerSample;
                voice.phase -= std::floor(voice.phase);
            }
            const float sample = raw * voice.amp * voice.env;
            mMix[static_cast<size_t>(i) * kChannels] += sample * voice.gainL;
            mMix[static_cast<size_t>(i) * kChannels + 1] += sample * voice.gainR;

            voice.freq += freqStep;
            voice.amp += ampStep;
            voice.gainL += gainLStep;
            voice.gainR += gainRStep;
        }

        voice.freq = voice.targetFreq;
        voice.amp = voice.targetAmp;
        voice.gainL = voice.targetGainL;
        voice.gainR = voice.targetGainR;
        if (voice.stopping && voice.env <= 0.0f) {
            voice.active = false;
        }
    }

    mOut.resize(mMix.size());
    for (size_t i = 0; i < mMix.size(); ++i) {
        mOut[i] = static_cast<int16_t>(std::clamp(mMix[i], -1.0f, 1.0f) * kInt16Peak);
    }
}

void CueService::Tick() {
    if (!mStream) {
        return;
    }
    // Nothing is queued while idle, on purpose: a primed buffer would delay the next cue by
    // however much silence sits in front of it.
    const bool anyActive =
        std::any_of(std::begin(mVoices), std::end(mVoices), [](const Voice& v) { return v.active; });
    if (!anyActive) {
        return;
    }

    const int queuedBytes = SDL_GetAudioStreamQueued(mStream);
    if (queuedBytes < 0) {
        return;
    }
    // Bounded by the queue depth itself: what is already queued can only reduce this, so a stall
    // cannot turn into a burst of stale audio.
    const int queuedFrames = queuedBytes / (kChannels * static_cast<int>(sizeof(int16_t)));
    const int needed = kTargetQueueSamples - queuedFrames;
    if (needed <= 0) {
        return;
    }

    Render(needed);
    if (!SDL_PutAudioStreamData(mStream, mOut.data(),
                                static_cast<int>(mOut.size() * sizeof(int16_t)))) {
        // Otherwise every cue in the mod goes silent with nothing to say why.
        RT_LOGF(RT_TAG_A11Y, "cue audio: SDL_PutAudioStreamData failed: %s\n", SDL_GetError());
    }
}

}  // namespace a11y::audio
