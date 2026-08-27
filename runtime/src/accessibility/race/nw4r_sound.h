#ifndef MKW_ACCESSIBILITY_RACE_NW4R_SOUND_H
#define MKW_ACCESSIBILITY_RACE_NW4R_SOUND_H

#include <cstdint>

namespace a11y::race {

// Offsets on nw4r::snd::detail::BasicSound shared by the volume services. Header-shared for the
// same reason as kPointerStride: the unity build merges anonymous namespaces, so a constant two
// files need must live in exactly one place.

// SetInitialVolume (0x8008F530) writes this f32. UpdateParam (0x8008EF20) multiplies it in as the
// first factor of the effective volume and nothing per-frame rewrites it - the volume analogue of
// the external pan at +0xA8. No clamp of its own; the end product saturates at Voice::SetVolume
// (0x800AA860).
inline constexpr std::uint32_t kSoundInitialVolume = 0xA4;

// The sound's archive id, a u32 written by BasicSound::SetId (0x8008F8D0). What identifies a
// sound on a shared handle.
inline constexpr std::uint32_t kSoundIdOffset = 144;

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_NW4R_SOUND_H
