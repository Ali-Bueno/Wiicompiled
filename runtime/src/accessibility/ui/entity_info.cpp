#include "entity_info.h"

#include <cstddef>

#include "accessibility/localization.h"
#include "class_probe.h"
#include "machine_graph.h"
#include "memory.h"

namespace a11y::ui {
namespace {

// Every address here comes from projects/mkwii/MAP.txt and names the symbol it was taken from.

// The two buttons worth describing, each named by a method only its class implements.
constexpr std::uint32_t kDriverButtonGetClassName = 0x807E3E00;   // ButtonDriver::GetClassName
constexpr std::uint32_t kVehicleButtonGetClassName = 0x808448DC;  // ButtonMachine::GetClassName

// PushButton's entity id: the character for a driver button, the vehicle for a machine one. Written
// by CtrlMenuCharacterSelect::LoadButton (func_807E2928) and Pages::KartSelect::InitButtonMachine
// (func_80847344), and searched on by GetButtonDriver (func_807E35B0) and GetButtonMachineById
// (func_808471C4). It lives on the shared PushButton base, which is why one offset serves both
// screens.
constexpr std::uint32_t kPushButtonEntityId = 576;
constexpr std::int32_t kNoEntity = -1;  // a locked character, or an empty slot in the grid

// GetKartWeightClass (func_8081CB70) and GetCharacterWeightClass (func_8081CD3C) are leaf scans over
// these six int32 tables, searched light, medium, heavy. They compare values rather than index by
// id, so an id in none of them simply falls through and no read can leave a table. Scanning them
// here needs no CpuContext, so the mod never has to call into the game for it.
struct WeightTable {
    std::uint32_t address;
    std::uint32_t count;
};
constexpr WeightTable kVehicleWeight[] = {{0x808AB770, 12}, {0x808AB7A0, 12}, {0x808AB7D0, 12}};
constexpr WeightTable kDriverWeight[] = {{0x808AB800, 15}, {0x808AB83C, 17}, {0x808AB880, 16}};
constexpr const char* kWeightKeys[] = {"weight_light", "weight_medium", "weight_heavy"};

std::string WeightClass(const WeightTable (&tables)[3], std::int32_t id) {
    for (std::size_t klass = 0; klass < 3; ++klass) {
        for (std::uint32_t i = 0; i < tables[klass].count; ++i) {
            std::uint32_t entry = 0;
            if (Memory::TryRead32(tables[klass].address + i * sizeof(std::uint32_t), entry) &&
                static_cast<std::int32_t>(entry) == id) {
                return a11y::loc::Get(kWeightKeys[klass]);
            }
        }
    }
    return {};
}

void Append(std::string& sentence, const std::string& part) {
    if (part.empty()) {
        return;
    }
    if (!sentence.empty()) {
        sentence += ". ";
    }
    sentence += part;
}

}  // namespace

std::string DescribeFocusedEntity(const std::vector<std::uint32_t>& layers, std::uint32_t focused) {
    if (focused == 0) {
        return {};
    }
    const bool driver = ImplementsMethod(focused, kDriverButtonGetClassName);
    const bool vehicle = !driver && ImplementsMethod(focused, kVehicleButtonGetClassName);
    if (!driver && !vehicle) {
        return {};
    }

    std::uint32_t stored = 0;
    if (!Memory::TryRead32(focused + kPushButtonEntityId, stored)) {
        return {};
    }
    const std::int32_t id = static_cast<std::int32_t>(stored);
    if (id == kNoEntity) {
        // A sighted player sees the slot is not available, so it is said rather than left silent.
        return a11y::loc::Get("entity_locked");
    }

    std::string sentence;
    Append(sentence, driver ? WeightClass(kDriverWeight, id) : WeightClass(kVehicleWeight, id));
    if (vehicle) {
        // Only the vehicle screens carry a stat graph; character select has none to read.
        Append(sentence, DescribeVehicleStats(layers, id));
    }
    return sentence;
}

}  // namespace a11y::ui
