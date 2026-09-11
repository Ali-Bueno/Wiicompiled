#ifndef MKW_ACCESSIBILITY_MII_MII_DATA_H
#define MKW_ACCESSIBILITY_MII_MII_DATA_H

#include <cstddef>
#include <cstdint>

// RFLiCharData: the 74-byte raw Mii record. Layout from RFL::iConvertRaw2InfoCore (0x800C6C60),
// accepted ranges from RFL::iCheckValidInfo (0x800CA8C0), both in the game's RFL library.
// Everything here is verified against the PAL DOL; see docs/mii-name.md.
namespace a11y::mii {

// RFLiGetDefaultData (0x800CA7D0): entry i is at 0x80250000 - 15152 + i * 74, i < 6. The six
// "no name" Miis the game builds its licences from; nothing in the game ever writes them.
inline constexpr std::uint32_t kDefaultTable = 0x8024C4D0;
inline constexpr std::uint32_t kDefaultCount = 6;
inline constexpr std::uint32_t kCharDataSize = 74;

// Name: 10 UTF-16BE units, zero padded, no terminator. The first unit must not be zero
// (RFL rejects a nameless Mii).
inline constexpr std::uint32_t kNameOffset = 0x02;
inline constexpr std::size_t kNameUnits = 10;

// RFLCreateID, 8 bytes. RFL::SearchDefaultData (0x800CA820) finds a licence's Mii in the table
// by this id, so it must never change.
inline constexpr std::uint32_t kCreateIdOffset = 0x18;
inline constexpr std::size_t kCreateIdSize = 8;

// One field of the record: a run of bits inside a big-endian 16- or 32-bit word (or a whole
// byte), numbered from the most significant bit like the RFL code numbers them. `max` is the
// largest value RFL::iCheckValidInfo accepts.
struct BitField {
    std::uint32_t offset;
    std::uint32_t wordBits;  // 8, 16 or 32
    std::uint32_t firstBit;
    std::uint32_t bits;
    std::uint32_t max;
};

// The fields that decide what the Mii looks like: types and colours. Scales, positions and
// rotations are deliberately left at the default entry's values, which sit in the middle of
// their ranges, so a random Mii is still a normal-looking one. Never listed here: the invalid
// flag (0x00 bit 0), birthday (month and day are coupled), the special flag (0x20 bit 13), the
// createID and the creator name.
inline constexpr BitField kLookFields[] = {
    {0x00, 16, 1, 1, 1},     // gender
    {0x00, 16, 11, 4, 11},   // favourite colour
    {0x16, 8, 0, 8, 128},    // height
    {0x17, 8, 0, 8, 128},    // weight
    {0x20, 16, 0, 3, 7},     // face shape
    {0x20, 16, 3, 3, 5},     // skin colour
    {0x22, 16, 0, 7, 71},    // hair type
    {0x22, 16, 7, 3, 7},     // hair colour
    {0x22, 16, 10, 1, 1},    // hair parting flipped
    {0x24, 32, 0, 5, 23},    // eyebrow type
    {0x24, 32, 16, 3, 7},    // eyebrow colour
    {0x28, 32, 0, 6, 47},    // eye type
    {0x28, 32, 16, 3, 5},    // eye colour
    {0x2C, 16, 0, 4, 11},    // nose type
    {0x2E, 16, 0, 5, 23},    // mouth type
    {0x2E, 16, 5, 2, 2},     // lip colour
    {0x30, 16, 0, 4, 8},     // glasses type (0 = none)
    {0x30, 16, 4, 3, 5},     // glasses colour
    {0x32, 16, 0, 2, 3},     // moustache type (0 = none)
    {0x32, 16, 2, 2, 3},     // beard type (0 = none)
    {0x32, 16, 4, 3, 7},     // facial hair colour
    {0x34, 16, 0, 1, 1},     // mole
};

// LicenseMgr, the four licences in RAM. `mgr = *(u32*)0x809BD748`; licences start at mgr+0x38 with
// this stride; the selected one is the s16 at mgr+0x36 (LicenseMgr::GetMiiName 0x80546FDC returns
// the licence itself: the name is its first field, 22 bytes, then the RFLCreateID).
inline constexpr std::uint32_t kLicenseMgrPtr = 0x809BD748;
inline constexpr std::uint32_t kLicensesOffset = 0x38;
inline constexpr std::uint32_t kLicenseStride = 0x93F0;
inline constexpr std::uint32_t kCurrentLicenseOffset = 0x36;
inline constexpr std::uint32_t kLicenseNameOffset = 0x00;
inline constexpr std::uint32_t kLicenseCreateIdOffset = 0x16;

// The save-file copy (rksys.dat RKPD blocks) the game flushes on its next save: `rkpd = *(mgr+0x14)`,
// one block per licence, name at +0x1C and createID at +0x30 (LicenseMgr::WriteToRKPD 0x805467D0).
inline constexpr std::uint32_t kRkpdPtrOffset = 0x14;
inline constexpr std::uint32_t kRkpdStride = 0x8CC0;
inline constexpr std::uint32_t kRkpdNameOffset = 0x1C;
inline constexpr std::uint32_t kRkpdCreateIdOffset = 0x30;

// The RFL manager, the global RFL::SearchOfficialData (0x800C75F0) reads. The NAND Mii database
// (RFL_DB.dat) lives at manager+0x10: RFL::Init allocates and formats it whether or not the file
// opens, and this port never ships the file, so it is empty and flagged broken.
inline constexpr std::uint32_t kRflManagerPtr = 0x80386298;
inline constexpr std::uint32_t kRflDatabaseOffset = 0x10;
// RFLiSetFileBroken's byte: RFLiGetCharData answers NULL for every slot while any bit is set.
inline constexpr std::uint32_t kRflFileStateOffset = 6972;
// 'RNOD' magic, then 100 records of kCharDataSize; a slot is live when its createID is non-zero.
inline constexpr std::uint32_t kDatabaseRecordsOffset = 4;
inline constexpr std::uint32_t kDatabaseSlot = 0;
// createID byte 0: 0x80 marks a built-in origin, 0x20 a special Mii (RFL::iIsSameID refuses it).
// The game shows "Player" instead of the name for a Mii it recognises as built-in, on every
// console, so a Mii of our own needs both bits clear.
inline constexpr std::uint8_t kCreateIdSpecialBits = 0x80 | 0x20;

}  // namespace a11y::mii

#endif  // MKW_ACCESSIBILITY_MII_MII_DATA_H
