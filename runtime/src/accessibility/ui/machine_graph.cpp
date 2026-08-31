#include "machine_graph.h"

#include <algorithm>
#include <array>
#include <cstddef>

#include "accessibility/localization.h"
#include "class_probe.h"
#include "layers.h"
#include "memory.h"
#include "page_controls.h"

namespace a11y::ui {
namespace {

// Every address here comes from projects/mkwii/MAP.txt and names the symbol it was taken from.

// CtrlMenuMachineGraph, found by class rather than by an offset into the page. The page does embed
// it at a fixed offset, but it also registers it with Page::AddControl (func_8060246C) like any
// other control, so a walk of the control list finds it - and that one lookup then serves single
// player, battle and multiplayer kart select without the reader knowing which page it is on.
constexpr std::uint32_t kMachineGraphGetClassName = 0x807E7A2C;  // CtrlMenuMachineGraph::GetClassName

// CtrlMenuMachineGraph::Load (func_807E7A38) builds two arrays of row pointers out of
// parameter/machine_para.bin: one row per driver, one per vehicle.
constexpr std::uint32_t kDriverRowArray = 376;
constexpr std::uint32_t kVehicleRowArray = 380;
constexpr std::uint32_t kDriverRowCount = 27;
constexpr std::uint32_t kVehicleRowCount = 36;

// The seven bars, in the order of the name table at 0x808A8C70: Speed, Weight, Accel, Handling,
// Drift, Dirt, Miniturbo. That table is the game's own, so the order is read rather than assumed.
constexpr std::size_t kStatCount = 7;
constexpr std::size_t kSpeedIndex = 0;
constexpr const char* kStatKeys[kStatCount] = {"stat_speed",     "stat_weight",   "stat_acceleration",
                                               "stat_handling",  "stat_drift",    "stat_offroad",
                                               "stat_miniturbo"};

// How OnLoad turns the tick sum into the bar's fill: a bias of five, a second five for speed alone,
// over a divisor that is wider for speed than for the rest. The game's numbers, not ours - the 5.0f
// is the eighth word of that same name table.
constexpr float kBias = 5.0f;
constexpr float kSpeedExtraBias = 5.0f;
constexpr float kSpeedDivisor = 35.0f;
constexpr float kDivisor = 30.0f;
constexpr float kFullBar = 100.0f;

// The driver the bars are drawn against, read the way Pages::KartSelect::OnExternalButtonSelect
// (func_80846EA4) builds OnLoad's argument: the SectionMgr singleton, its menu data, then the
// per-player character id. A raw character id - OnLoad folds the Mii ones itself.
constexpr std::uint32_t kSectionMgrMenuData = 152;
constexpr std::uint32_t kMenuDataCharacterIds = 300;
constexpr std::uint32_t kPlayerOne = 0;  // the reader follows player one only, as the focus does

// The 24 named characters take a row each; the last three rows are the Mii sizes, and these are the
// id ranges OnLoad folds onto them.
constexpr std::int32_t kNamedDriverCount = 24;
struct MiiRange {
    std::int32_t first;
    std::int32_t last;
    std::uint32_t row;
};
constexpr MiiRange kMiiRanges[] = {{24, 27, 24}, {30, 33, 25}, {36, 39, 26}};

std::uint32_t FindGraph(const std::vector<std::uint32_t>& layers) {
    for (const std::uint32_t layer : layers) {
        for (const std::uint32_t control : PageControls(layer)) {
            if (ImplementsMethod(control, kMachineGraphGetClassName)) {
                return control;
            }
        }
    }
    return 0;
}

bool DriverRow(std::int32_t characterId, std::uint32_t& row) {
    if (characterId < 0) {
        return false;
    }
    if (characterId < kNamedDriverCount) {
        row = static_cast<std::uint32_t>(characterId);
        return true;
    }
    for (const MiiRange& range : kMiiRanges) {
        if (characterId >= range.first && characterId <= range.last) {
            row = range.row;
            return true;
        }
    }
    return false;  // a Mii id OnLoad's own fold does not cover: better silent than wrong
}

bool CurrentDriverId(std::int32_t& id) {
    const std::uint32_t manager = SectionManager();
    std::uint32_t menuData = 0;
    std::uint32_t stored = 0;
    if (manager == 0 || !Memory::TryRead32(manager + kSectionMgrMenuData, menuData) ||
        menuData == 0 ||
        !Memory::TryRead32(menuData + kMenuDataCharacterIds + kPlayerOne * sizeof(std::uint32_t),
                           stored)) {
        return false;
    }
    id = static_cast<std::int32_t>(stored);
    return true;
}

// Seven signed 16-bit ticks packed into fourteen bytes. TryRead32 is the only non-throwing accessor,
// so they come out of four words; the last is taken from +10 rather than +12 to keep every read
// inside the row.
bool ReadRow(std::uint32_t array, std::uint32_t index, std::uint32_t count,
             std::array<std::int32_t, kStatCount>& out) {
    if (array == 0 || index >= count) {
        return false;
    }
    std::uint32_t row = 0;
    if (!Memory::TryRead32(array + index * sizeof(std::uint32_t), row) || row == 0) {
        return false;
    }

    constexpr std::size_t kWordCount = 4;
    constexpr std::uint32_t kWordOffsets[kWordCount] = {0, 4, 8, 10};
    std::uint32_t words[kWordCount] = {};
    for (std::size_t w = 0; w < kWordCount; ++w) {
        if (!Memory::TryRead32(row + kWordOffsets[w], words[w])) {
            return false;
        }
    }
    for (std::size_t i = 0; i < kStatCount - 1; ++i) {
        const std::uint32_t word = words[i / 2];
        out[i] = static_cast<std::int16_t>((i % 2) == 0 ? word >> 16 : word);
    }
    out[kStatCount - 1] = static_cast<std::int16_t>(words[3]);
    return true;
}

int BarPercent(std::int32_t ticks, std::size_t stat) {
    const bool speed = stat == kSpeedIndex;
    const float bias = kBias + (speed ? kSpeedExtraBias : 0.0f);
    const float fill = (static_cast<float>(ticks) + bias) / (speed ? kSpeedDivisor : kDivisor);
    return std::clamp(static_cast<int>(fill * kFullBar + 0.5f), 0, static_cast<int>(kFullBar));
}

}  // namespace

std::string DescribeVehicleStats(const std::vector<std::uint32_t>& layers, std::int32_t vehicleId) {
    const std::uint32_t graph = FindGraph(layers);
    if (graph == 0) {
        return {};
    }

    std::int32_t characterId = 0;
    std::uint32_t driverIndex = 0;
    if (!CurrentDriverId(characterId) || !DriverRow(characterId, driverIndex)) {
        return {};
    }

    std::uint32_t driverArray = 0;
    std::uint32_t vehicleArray = 0;
    std::array<std::int32_t, kStatCount> driver{};
    std::array<std::int32_t, kStatCount> vehicle{};
    if (!Memory::TryRead32(graph + kDriverRowArray, driverArray) ||
        !Memory::TryRead32(graph + kVehicleRowArray, vehicleArray) ||
        !ReadRow(driverArray, driverIndex, kDriverRowCount, driver) ||
        !ReadRow(vehicleArray, static_cast<std::uint32_t>(vehicleId), kVehicleRowCount, vehicle)) {
        return {};
    }

    std::string sentence = a11y::loc::Get("stats_intro");
    for (std::size_t i = 0; i < kStatCount; ++i) {
        sentence += ", ";
        sentence += a11y::loc::Format(
            "stat_value", {{"name", a11y::loc::Get(kStatKeys[i])},
                           {"n", std::to_string(BarPercent(driver[i] + vehicle[i], i))}});
    }
    return sentence;
}

}  // namespace a11y::ui
