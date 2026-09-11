#include "accessibility/mii/mii_identity.h"

#include <cstdint>

#include "accessibility/a11y_log.h"
#include "accessibility/guest_text.h"
#include "accessibility/mii/mii_data.h"
#include "accessibility/mii/mii_guest.h"
#include "accessibility/mii/mii_look.h"
#include "runtime_config.h"

namespace a11y::mii {
namespace {

Name g_name{};
CreateId g_createId{};
bool g_active = false;
// The record has to be rewritten whenever RFL reformats the database (it does once more when the
// missing RFL_DB.dat fails to open). A few lines say how often that happened; more would be a loop.
constexpr int kLoggedStores = 3;
int g_storesLogged = 0;

bool IsZero(const CreateId& id) {
    for (const std::uint8_t byte : id) {
        if (byte != 0) {
            return false;
        }
    }
    return true;
}

// Every entry gets the name and its own face: the six are still told apart where the game shows
// them side by side (Mii Select, Character Select), and entry 0 is the record copied below.
bool StampDefaultTable() noexcept {
    try {
        for (std::uint32_t i = 0; i < kDefaultCount; ++i) {
            const std::uint32_t entry = kDefaultTable + i * kCharDataSize;
            if (!WriteName(entry + kNameOffset, g_name, false)) {
                return false;
            }
            StampLook(entry, g_name, i);
        }
        return true;
    } catch (const Memory::AccessViolation&) {
        return false;
    }
}

// Our Mii lives in the NAND database, like one made in the Mii Channel: the licence resolves it
// there first (MiiManager::CreateStoreMii, 0x80527B0C), which is what keeps the built-in flag off.
void EnsureDatabaseRecord() {
    std::uint32_t manager = 0;
    std::uint32_t database = 0;
    if (!Memory::TryRead32(kRflManagerPtr, manager) || manager == 0 ||
        !Memory::TryRead32(manager + kRflDatabaseOffset, database) || database == 0) {
        return;
    }
    const std::uint32_t record = database + kDatabaseRecordsOffset + kDatabaseSlot * kCharDataSize;
    std::uint8_t fileState = 0;
    CreateId stored{};
    if (!ReadU8(manager + kRflFileStateOffset, fileState) ||
        !ReadCreateId(record + kCreateIdOffset, stored)) {
        return;
    }
    if (fileState == 0 && stored == g_createId) {
        return;
    }
    if (!CopyBytes(kDefaultTable, record, kCharDataSize) ||
        !WriteCreateId(record + kCreateIdOffset, g_createId) ||
        !WriteU8(manager + kRflFileStateOffset, 0)) {
        return;
    }
    if (g_storesLogged < kLoggedStores) {
        ++g_storesLogged;
        RT_LOGF(RT_TAG_A11Y, "mii: stored in the RFL database (slot %u, file state was %u)\n",
                kDatabaseSlot, fileState);
    }
}

// The licence keeps the createID and name it was made with. This port has no other source of
// Miis, so any licence not already on our id is moved to it: the built-in one it started with,
// or the id of an earlier name.
void MigrateLicense() {
    std::uint32_t manager = 0;
    if (!Memory::TryRead32(kLicenseMgrPtr, manager) || manager == 0) {
        return;
    }
    std::int16_t index = 0;
    try {
        index = static_cast<std::int16_t>(Memory::Read16(manager + kCurrentLicenseOffset));
    } catch (const Memory::AccessViolation&) {
        return;
    }
    if (index < 0) {
        return;
    }
    const std::uint32_t license =
        manager + kLicensesOffset + static_cast<std::uint32_t>(index) * kLicenseStride;
    CreateId current{};
    if (ReadCreateId(license + kLicenseCreateIdOffset, current) && !IsZero(current) &&
        current != g_createId && WriteCreateId(license + kLicenseCreateIdOffset, g_createId)) {
        WriteName(license + kLicenseNameOffset, g_name, true);
        RT_LOGF(RT_TAG_A11Y, "mii: licence %d moved to our Mii\n", index);
    }

    std::uint32_t rkpd = 0;
    if (!Memory::TryRead32(manager + kRkpdPtrOffset, rkpd) || rkpd == 0) {
        return;
    }
    const std::uint32_t block = rkpd + static_cast<std::uint32_t>(index) * kRkpdStride;
    if (ReadCreateId(block + kRkpdCreateIdOffset, current) && !IsZero(current) &&
        current != g_createId && WriteCreateId(block + kRkpdCreateIdOffset, g_createId)) {
        WriteName(block + kRkpdNameOffset, g_name, false);
    }
}

}  // namespace

void Init() {
    const std::u16string configured = Utf8ToUtf16(RuntimeConfigFile::MiiName());
    if (configured.empty()) {
        return;
    }
    for (std::size_t k = 0; k < g_name.size() && k < configured.size(); ++k) {
        g_name[k] = configured[k];
    }
    g_createId = CreateIdFor(g_name);
    g_active = StampDefaultTable();
    RT_LOGF(RT_TAG_A11Y, "mii: name \"%s\" %s\n", RuntimeConfigFile::MiiName().c_str(),
            g_active ? "stamped on the default Miis" : "could not be written; default Miis kept");
}

void Tick() {
    if (!g_active) {
        return;
    }
    // Both run every frame: the database record must be in place before the licence screen
    // builds its Miis, and the reads cost a dozen words.
    EnsureDatabaseRecord();
    MigrateLicense();
}

}  // namespace a11y::mii
