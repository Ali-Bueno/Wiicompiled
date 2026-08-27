#include "accessibility/audio/sample_bank.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_iostream.h>

#include "accessibility/a11y_log.h"
#include "accessibility/audio/cue_service.h"
#include "accessibility/localization.h"

namespace a11y::audio {
namespace {

constexpr const char* kSoundsDir = "accessibility_sounds";

const char* FileNameFor(SampleId id) {
    switch (id) {
        case SampleId::ItemBox:
            return "item_box.wav";
        case SampleId::Banana:
            return "banana.wav";
        default:
            return nullptr;
    }
}

// Decoded from memory: SDL_LoadWAV wants a UTF-8 path, and the exe can sit in a directory
// whose name does not survive the ANSI round trip.
bool DecodeWav(const std::filesystem::path& path, Sample& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        return false;
    }

    SDL_IOStream* io = SDL_IOFromConstMem(bytes.data(), bytes.size());
    if (!io) {
        return false;
    }
    SDL_AudioSpec srcSpec{};
    Uint8* srcData = nullptr;
    Uint32 srcLen = 0;
    if (!SDL_LoadWAV_IO(io, /*closeio=*/true, &srcSpec, &srcData, &srcLen)) {
        RT_LOGF(RT_TAG_A11Y, "sample bank: %s: %s\n", path.filename().string().c_str(),
                SDL_GetError());
        return false;
    }

    // Everything is held in the cue stream's own format, so Render can mix a sample and a
    // synthesized tone identically.
    SDL_AudioSpec dstSpec{};
    dstSpec.format = SDL_AUDIO_F32;
    dstSpec.channels = 1;
    dstSpec.freq = kSampleRate;

    Uint8* dstData = nullptr;
    int dstLen = 0;
    const bool converted = SDL_ConvertAudioSamples(&srcSpec, srcData, static_cast<int>(srcLen),
                                                   &dstSpec, &dstData, &dstLen);
    SDL_free(srcData);
    if (!converted) {
        RT_LOGF(RT_TAG_A11Y, "sample bank: %s: convert failed: %s\n",
                path.filename().string().c_str(), SDL_GetError());
        return false;
    }
    const float* samples = reinterpret_cast<const float*>(dstData);
    out.mono.assign(samples, samples + dstLen / sizeof(float));
    SDL_free(dstData);
    return !out.mono.empty();
}

}  // namespace

SampleBank& SampleBank::Instance() {
    static SampleBank instance;
    return instance;
}

void SampleBank::Load() {
    if (mLoaded) {
        return;
    }
    mLoaded = true;
    const std::filesystem::path dir = loc::ExeDirectory() / kSoundsDir;
    for (int i = 0; i < static_cast<int>(SampleId::Count); ++i) {
        const char* name = FileNameFor(static_cast<SampleId>(i));
        if (!name) {
            continue;
        }
        if (DecodeWav(dir / name, mSamples[i])) {
            RT_LOGF(RT_TAG_A11Y, "sample bank: %s loaded (%zu samples)\n", name,
                    mSamples[i].mono.size());
        } else {
            RT_LOGF(RT_TAG_A11Y, "sample bank: %s missing; that cue stays on the tone\n", name);
        }
    }
}

const Sample* SampleBank::Get(SampleId id) const {
    const int index = static_cast<int>(id);
    if (index <= static_cast<int>(SampleId::None) || index >= static_cast<int>(SampleId::Count) ||
        mSamples[index].mono.empty()) {
        return nullptr;
    }
    return &mSamples[index];
}

}  // namespace a11y::audio
