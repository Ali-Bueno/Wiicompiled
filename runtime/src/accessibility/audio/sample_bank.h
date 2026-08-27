#ifndef MKW_ACCESSIBILITY_AUDIO_SAMPLE_BANK_H
#define MKW_ACCESSIBILITY_AUDIO_SAMPLE_BANK_H

#include <vector>

namespace a11y::audio {

// Recorded cue sounds, carried over from the MK64 mod. The files are user-supplied local
// assets (accessibility_sounds\ next to the exe) and are never committed; a missing file
// simply leaves its cue on the synthesized fallback.
enum class SampleId {
    None,
    ItemBox,  // item_box.wav — the item box beacon blip
    Banana,   // banana.wav — reserved for the hazard beacon
    Count,
};

struct Sample {
    std::vector<float> mono;  // mono float PCM at the cue stream's own rate
};

class SampleBank {
public:
    static SampleBank& Instance();

    // Loads every known wav once. Safe to call again; it only loads the first time.
    void Load();

    // The decoded sample, or nullptr when the file was absent or unreadable.
    const Sample* Get(SampleId id) const;
    bool Has(SampleId id) const { return Get(id) != nullptr; }

private:
    SampleBank() = default;

    bool mLoaded = false;
    Sample mSamples[static_cast<int>(SampleId::Count)];
};

}  // namespace a11y::audio

#endif  // MKW_ACCESSIBILITY_AUDIO_SAMPLE_BANK_H
