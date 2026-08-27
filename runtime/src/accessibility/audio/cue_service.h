#ifndef MKW_ACCESSIBILITY_AUDIO_CUE_SERVICE_H
#define MKW_ACCESSIBILITY_AUDIO_CUE_SERVICE_H

#include <cstdint>
#include <vector>

#include "accessibility/audio/waveform.h"

struct SDL_AudioStream;

namespace a11y::audio {

// One voice per channel: a cue replaces whatever that channel was playing, and never cuts another
// family off. Carried over from the MK64 mod, where a curve beep silencing the track-limit tone
// left the player without edge feedback at exactly the moment it mattered.
enum class CueChannel {
    Edge,     // track limits: beeps near the edge, held tone past it
    Curve,    // curve approach, entry, apex and exit beeps
    ItemBox,  // nearest item box proximity blip
    Hazard,   // shells, bananas, obstacles
    Count,
};

// Long enough that a beep does not click, short enough to still feel immediate.
inline constexpr float kDefaultAttackSec = 0.005f;
inline constexpr float kDefaultReleaseSec = 0.010f;

struct CueSpec {
    Waveform shape = Waveform::Triangle;
    float frequencyHz = 0.0f;
    float amplitude = 0.0f;    // 0..1, before the master volume
    float pan = 0.0f;          // -1 hard left, 0 centre, +1 hard right
    float durationSec = 0.0f;  // one-shots only; SetSustained ignores it
    float attackSec = kDefaultAttackSec;
    float releaseSec = kDefaultReleaseSec;
};

// Our own SDL stream, deliberately not the game's audio path: audio_backend.h is a single queue,
// not a mixer, so pushing cues there would interleave them with the game's samples instead of
// mixing them. Owning the mix also makes pan and volume independent by construction.
class CueService {
public:
    static CueService& Instance();

    bool Start();
    void Shutdown();
    bool Available() const { return mStream != nullptr; }

    // Renders and tops up the queue. Nothing is heard unless this runs every frame.
    void Tick();

    void PlayOneShot(CueChannel channel, const CueSpec& spec);

    // Starts the channel if it is silent, otherwise re-steers pitch, pan and volume without
    // restarting it, so a caller can drive it every frame from a continuous value.
    void SetSustained(CueChannel channel, const CueSpec& spec);

    void Stop(CueChannel channel);
    void StopAll();

    void SetMasterVolume(float volume);

private:
    CueService() = default;

    struct Voice {
        bool active = false;
        bool sustained = false;
        bool stopping = false;
        Waveform shape = Waveform::Triangle;
        float phase = 0.0f;  // cycles; kept across re-steers so a pitch change does not click
        float freq = 0.0f, targetFreq = 0.0f;
        float amp = 0.0f, targetAmp = 0.0f;
        float gainL = 0.0f, targetGainL = 0.0f;
        float gainR = 0.0f, targetGainR = 0.0f;
        float env = 0.0f;  // attack/release envelope, 0..1
        float attackStep = 0.0f, releaseStep = 0.0f;  // per sample
        float remainingSec = 0.0f;
    };

    Voice& VoiceFor(CueChannel channel) { return mVoices[static_cast<int>(channel)]; }
    void Apply(Voice& voice, const CueSpec& spec, bool sustained, bool restart);
    void Render(int frames);

    SDL_AudioStream* mStream = nullptr;
    Voice mVoices[static_cast<int>(CueChannel::Count)];
    std::vector<float> mMix;
    std::vector<int16_t> mOut;
    float mMasterVolume = 1.0f;
};

}  // namespace a11y::audio

#endif  // MKW_ACCESSIBILITY_AUDIO_CUE_SERVICE_H
