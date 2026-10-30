#include <aurora/aurora.h>
#include <aurora/gfx.h>
#include <dolphin/vi.h>

#include <array>
#include <cmath>
#include <string>

#include "accessibility/menu/menu_rows.h"
#include "game_graphics_options.h"
#include "runtime_config.h"

// The F10 bar's graphics settings (settings_overlay.cpp DrawGraphicsSettings and the Resolution
// menu). Each row makes the same live call plus persist as the bar. The bar keeps its own
// cached copies of these values, which this menu cannot refresh, so the bar may show a stale
// value until the next launch.

namespace a11y::menu {
namespace {

// Resolution scales in the bar's order (settings_overlay.cpp kResolutions); 0 is the window size.
constexpr std::array<float, 8> kResolutionScales = {0.0f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f};
constexpr float kScaleTolerance = 0.001f;
constexpr float kHighFrameRateScale = 4.0f;
// First 6x entry; this one and the 8x after it are refused at a high frame rate.
constexpr size_t kFirstHighResolution = 6;
constexpr uint32_t kHighFrameRateFps = 60;
// Bloom's bit in the post-processing path mask (settings_overlay.cpp kEffectFlags).
constexpr uint32_t kBloomPathBit = 0x10u;

constexpr std::array<uint32_t, 3> kInterpolationFps = {0, 120, 180};
constexpr std::array<const char*, 3> kDisplayModeNames = {"windowed", "borderless", "exclusive"};

bool IsHighResolution(float scale) {
    return std::fabs(scale - 6.0f) < kScaleTolerance || std::fabs(scale - 8.0f) < kScaleTolerance;
}

int ResolutionIndex() {
    const float scale = RuntimeConfigFile::ResolutionMultiplier(1.0f);
    for (size_t i = 0; i < kResolutionScales.size(); ++i) {
        if (std::fabs(kResolutionScales[i] - scale) < kScaleTolerance) {
            return static_cast<int>(i);
        }
    }
    return 1;
}

int InterpolationIndex() {
    const uint32_t fps = RuntimeConfigFile::FrameInterpolationFps(0);
    for (size_t i = 0; i < kInterpolationFps.size(); ++i) {
        if (kInterpolationFps[i] == fps) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

bool HighFrameRate() {
    return RuntimeConfigFile::FrameInterpolationFps(0) > kHighFrameRateFps;
}

void SetResolutionScale(float scale) {
    VISetFrameBufferScale(scale);
    RuntimeConfigFile::SetResolutionMultiplier(scale);
}

void SetResolution(int index) {
    // Refused, not skipped: right at 4x stays at 4x while a high frame rate is on.
    if (HighFrameRate() && static_cast<size_t>(index) >= kFirstHighResolution) {
        return;
    }
    SetResolutionScale(kResolutionScales[static_cast<size_t>(index)]);
}

void SetInterpolation(int index) {
    const uint32_t fps = kInterpolationFps[static_cast<size_t>(index)];
    aurora_set_frame_interpolation_fps(fps);
    RuntimeConfigFile::SetFrameInterpolationFps(fps);
    if (HighFrameRate() && IsHighResolution(RuntimeConfigFile::ResolutionMultiplier(1.0f))) {
        SetResolutionScale(kHighFrameRateScale);
    }
    if (aurora_get_display_mode() == AURORA_DISPLAY_MODE_EXCLUSIVE) {
        // Re-applied so the display refresh tracks the new target.
        aurora_set_display_mode(AURORA_DISPLAY_MODE_EXCLUSIVE);
    }
}

void SetDisplayMode(int index) {
    const auto mode = static_cast<AuroraDisplayMode>(index);
    aurora_set_display_mode(mode);
    // Persisted only if the mode took (an exclusive request can be refused).
    if (aurora_get_display_mode() == mode) {
        RuntimeConfigFile::SetDisplayMode(kDisplayModeNames[static_cast<size_t>(index)]);
    }
}

void SetBloomDisabled(bool disabled) {
    uint32_t mask = RuntimeConfigFile::DisabledPostProcessingPaths(0);
    mask = disabled ? (mask | kBloomPathBit) : (mask & ~kBloomPathBit);
    RuntimeGameGraphicsOptions::SetDisabledPostProcessingPaths(mask);
    RuntimeConfigFile::SetDisabledPostProcessingPaths(mask);
}

}  // namespace

std::vector<MenuRow> GraphicsRows() {
    return {
        Choice("opt_resolution",
               {"opt_res_auto", "opt_res_native", "opt_res_1_5x", "opt_res_2x", "opt_res_3x",
                "opt_res_4x", "opt_res_6x", "opt_res_8x"},
               ResolutionIndex, SetResolution),
        Switch("opt_disable_bloom",
               [] { return (RuntimeConfigFile::DisabledPostProcessingPaths(0) & kBloomPathBit) != 0; },
               SetBloomDisabled),
        Choice("opt_display_mode", {"opt_display_windowed", "opt_display_borderless", "opt_display_exclusive"},
               [] { return static_cast<int>(aurora_get_display_mode()); }, SetDisplayMode),
        Choice("opt_frame_interp", {"opt_interp_off", "opt_interp_120", "opt_interp_180"},
               InterpolationIndex, SetInterpolation),
        Switch("opt_disable_copy_filter", [] { return RuntimeConfigFile::DisableCopyFilter(true); },
               [](bool on) {
                   aurora_set_disable_copy_filter(on);
                   RuntimeConfigFile::SetDisableCopyFilter(on);
               }),
        Switch("opt_skip_unready", [] { return RuntimeConfigFile::SkipUnreadyPipelines(true); },
               [](bool on) {
                   aurora_set_skip_unready_pipelines(on);
                   RuntimeConfigFile::SetSkipUnreadyPipelines(on);
               }),
    };
}

}  // namespace a11y::menu
