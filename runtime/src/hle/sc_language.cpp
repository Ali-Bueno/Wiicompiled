// Wii system language.
//
// Upstream stubs SCGetProductArea, SCGetAspectRatio and SCGetEuRgb60Mode in sc.cpp but not
// SCGetLanguage, so the recompiled SDK code runs for real: it asks NAND for SYSCONF, the managed
// NAND never seeds one (it only seeds FaceLib, nand_isfs.cpp:334), the read fails - those are the
// "[nand] NANDOpen: FAILED" lines at boot - and the SDK falls back to its default of Japanese. A
// PAL disc then shows Japanese menus.
//
// Kept in its own file rather than added to sc.cpp so an upstream sync never conflicts here.

#include "hle_stubs.h"

#include <cstdint>

namespace {

// SC_LANG_* from the Wii SDK, the same enumeration SYSCONF stores in IPL.LNG:
// 0 JP, 1 EN, 2 DE, 3 FR, 4 ES, 5 IT, 6 NL, 7 SCH, 8 TCH, 9 KO.
// PAL RMCP01 carries EN, FR, DE, ES and IT; asking for anything else falls back inside the game.
constexpr uint32_t kLanguageSpanish = 4;

}  // namespace

extern "C" uint32_t SCGetLanguage_HLE() {
    return kLanguageSpanish;
}

PPC_NATIVE_OVERRIDE(801B1D0C, SCGetLanguage_HLE, uint32_t, (), ());
