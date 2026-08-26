// Wii system language.
//
// Upstream stubs SCGetProductArea, SCGetAspectRatio and SCGetEuRgb60Mode in sc.cpp but not
// SCGetLanguage, so the recompiled SDK code runs for real: it asks NAND for SYSCONF, the managed
// NAND never seeds one (it only seeds FaceLib, nand_isfs.cpp:334), the read fails - those are the
// "[nand] NANDOpen: FAILED" lines at boot - and the SDK falls back to its own default. The player
// then gets a language they never chose and cannot change.
//
// Kept in its own file rather than added to sc.cpp so an upstream sync never conflicts here.

#include "hle_stubs.h"

#include <cstdint>

#include "runtime_config.h"

extern "C" uint32_t SCGetLanguage_HLE() {
    // Read per call rather than cached: the config is loaded long before the game boots, and this
    // way a value written by the settings UI is picked up on the next launch with no extra wiring.
    return static_cast<uint32_t>(RuntimeConfigFile::SystemLanguage());
}

PPC_NATIVE_OVERRIDE(801B1D0C, SCGetLanguage_HLE, uint32_t, (), ());
