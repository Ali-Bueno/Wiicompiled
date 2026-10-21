#include "accessibility/audio/cue_service.h"

#include <algorithm>
#include <cmath>

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_init.h>

#include "accessibility/a11y_log.h"

namespace a11y::audio {
namespace {

// Three frames of audio kept queued. The tick that refills this is one frame apart, so three
// survives a hitch, and the resulting latency stays under the ~100 ms where a cue starts to feel
// detached from what caused it.
constexpr int kQueuedFrames = 3;

// The guest runs at one of two video modes; anything outside that range is a stall being measured,
// not a frame period, and must not turn into seconds of queued audio.
constexpr float kMinFramePeriodSec = kDefaultFramePeriodSec;  // NTSC 60 Hz
constexpr float kMaxFramePeriodSec = 1.0f / 50.0f;            // PAL 50 Hz, the mode RMCP01 runs at

// Pitch is a playback-rate multiplier, so zero would freeze the read position: a sample-backed
// one-shot would never reach its end and never stop. Two octaves down is the lowest transposition
// a cue is still recognizable at, and it bounds a one-shot at four times its own length.
constexpr float kMinPlaybackRate = 0.25f;

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
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    if (!SDL_ResumeAudioStreamDevice(stream)) {
        RT_LOGF(RT_TAG_A11Y, "cue audio: SDL_ResumeAudioStreamDevice failed: %s\n", SDL_GetError());
        SDL_DestroyAudioStream(stream);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
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
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
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

void CueService::SetChannelVolume(CueChannel channel, float volume) {
    VoiceFor(channel).channelVolume = std::clamp(volume, 0.0f, 1.0f);
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
    const Sample* sample = SampleBank::Instance().Get(spec.sample);
    voice.rate = std::max(spec.pitch, kMinPlaybackRate);
    // Rewind on a restart, on a re-activation from silence, on a swap of sample, and whenever the
    // position is no longer playable: a voice parked past its own end never advances again.
    if (restart || !voice.active || sample != voice.sample ||
        (sample != nullptr && !SamplePlayable(*sample, voice.samplePos))) {
        voice.samplePos = 0.0f;
    }
    voice.sample = sample;
    voice.sweepPerSample = 1.0f;
    if (!sustained) {
        voice.remainingSec = spec.durationSec;
        if (!sample && spec.sweepToHz > 0.0f && spec.frequencyHz > 0.0f && spec.durationSec > 0.0f) {
            voice.sweepPerSample = std::pow(spec.sweepToHz / spec.frequencyHz,
                                            1.0f / (spec.durationSec * static_cast<float>(kSampleRate)));
            voice.targetFreq = spec.sweepToHz;
            voice.freq = spec.frequencyHz;
        }
    }

    // A silent channel has nothing to glide from, so it starts exactly where it was asked to.
    if (restart || !voice.active) {
        if (voice.sweepPerSample == 1.0f) {
            voice.freq = voice.targetFreq;
        }
        voice.amp = voice.targetAmp;
        voice.gainL = voice.targetGainL;
        voice.gainR = voice.targetGainR;
        // Re-activating from silence is a start, not a re-steer: a stale phase, envelope or read
        // position left behind by the previous cue would click, swallow the attack, or stay mute.
        voice.phase = 0.0f;
        voice.env = 0.0f;
        voice.active = true;
    }
}

void CueService::SetFramePeriod(float seconds) {
    mFrameSec = std::clamp(seconds, kMinFramePeriodSec, kMaxFramePeriodSec);
}

int CueService::TargetQueueFrames() const {
    return static_cast<int>(static_cast<float>(kSampleRate) * mFrameSec *
                            static_cast<float>(kQueuedFrames));
}

// Samples still owed to the longest release ramp in flight. Rendering fewer would leave a
// half-faded tone at the end of the queue - exactly the click the ramp exists to avoid.
int CueService::ReleaseTailFrames() const {
    int tail = 0;
    for (const Voice& voice : mVoices) {
        if (!voice.active || !voice.stopping || voice.releaseStep <= 0.0f) {
            continue;
        }
        tail = std::max(tail, static_cast<int>(std::ceil(voice.env / voice.releaseStep)));
    }
    return tail;
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
    int needed = TargetQueueFrames() - queuedFrames;
    // A release must land in the queue whole; stopping halfway leaves a truncated tone to be heard
    // if the queue then drains before the next tick.
    needed = std::max(needed, ReleaseTailFrames());
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
