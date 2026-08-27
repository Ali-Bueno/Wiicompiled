#include "accessibility/race/kmp_reader.h"

#include "accessibility/a11y_log.h"
#include "accessibility/race/course_map.h"
#include "accessibility/race/guest_read.h"

namespace a11y::race {
namespace {

// Recovered from the translated bodies, never guessed.
//
// KMP::Manager::CreateInstance (0x80512694) stores the instance here and DestroyInstance
// (0x8051271C) writes zero back. Those are the only two writers of this word in the entire
// translation, so null means "no course loaded" exactly rather than approximately.
constexpr std::uint32_t kKmpManagerPtr = 0x809BD6E8;

// KMP::Manager::Init (0x805127EC) parses the sections in file order and stores each one here.
constexpr std::uint32_t kManagerCkptSection = 0x20;
constexpr std::uint32_t kManagerEnptSection = 0x10;

// Shape of a parsed section, proved by the Get*Count accessors: pointer array, then a u16 count.
constexpr std::uint32_t kSectionHolderArray = 0x00;
constexpr std::uint32_t kSectionCount = 0x04;

// KMP::Holder<CKPT> keeps the raw entry at +0, and caches a midpoint and a unit normal the game
// computes once at parse time.
constexpr std::uint32_t kHolderRawEntry = 0x00;

// The raw CKPT entry: two edge points in the horizontal plane, 20 bytes per entry.
constexpr std::uint32_t kCkptLeftX = 0x00;
constexpr std::uint32_t kCkptLeftZ = 0x04;
constexpr std::uint32_t kCkptRightX = 0x08;
constexpr std::uint32_t kCkptRightZ = 0x0C;

// KMP::Holder<ENPT> keeps the raw entry at +0x04, not at +0x00 like the checkpoint holder does.
// InitLinks (0x80516D74) then allocates two exact-sized index arrays on the holder and fills them
// from the ENPH groups, which is why ENPH itself never has to be parsed here.
constexpr std::uint32_t kEnptHolderRawEntry = 0x04;
constexpr std::uint32_t kEnptHolderNextArray = 0x0C;
constexpr std::uint32_t kEnptHolderNextCount = 0x11;
constexpr std::uint32_t kEnptX = 0x00;
constexpr std::uint32_t kEnptY = 0x04;
constexpr std::uint32_t kEnptZ = 0x08;
constexpr std::uint32_t kEnptRange = 0x0C;

// KMP::Manager::Init stores the ITPT section at +0x18 (it writes manager+0x18 straight after
// ParseITPT, and KMP::Manager::GetITPTCount (0x80512CEC) reads it back). The section object has
// the same shape as the others, but KMP::Holder<ITPT> is NOT the ENPT holder under another name:
// its raw entry pointer sits at +0x00 and its link indices are INLINE u8[6] arrays rather than
// heap arrays - prev at +0x04, next at +0x0A, counts at +0x10/+0x11. Proof: the holder's own
// InitLinks (0x80517E88, no allocator calls, prefills 12 bytes at +0x04 with 0xFF) and the
// GetNextITPT/GetNextITPTCount accessors (0x805181F0 reads holder+0x0A+i, 0x80518268 reads
// holder+0x11). The raw ITPT entry shares the ENPT prefix: position, then the corridor float.
constexpr std::uint32_t kManagerItptSection = 0x18;
constexpr std::uint32_t kItptHolderRawEntry = 0x00;
constexpr std::uint32_t kItptHolderNextInline = 0x0A;
constexpr std::uint32_t kItptHolderNextCount = 0x11;

// The start-point section. Raceinfo::GetStartENPT (0x80536828) reads a byte cached on its first
// holder by KMP::Holder<KTPT>::InitLinks, which is the only writer of it.
constexpr std::uint32_t kManagerKtptSection = 0x08;
constexpr std::uint32_t kKtptHolderStartEnpt = 0x04;

// KMP::CKPTSection::Init stores the lap length CalcTotalDistance returned here.
constexpr std::uint32_t kCkptSectionLapLength = 0x10;

// An unset link index. InitLinks prefills both arrays with this before filling them.
constexpr std::uint8_t kNoRouteLink = 0xFF;


// A course with more checkpoints than this is not a course; the read is bounded so a bad pointer
// cannot spin the frame tick.
constexpr std::uint32_t kMaxCheckpoints = 1024;

}  // namespace

std::uint32_t CourseToken() {
    std::uint32_t manager = 0;
    return Memory::TryRead32(kKmpManagerPtr, manager) ? manager : 0;
}

std::uint64_t CourseSignature() {
    std::uint32_t manager = CourseToken();
    std::uint32_t section = 0;
    std::uint32_t holders = 0;
    std::uint32_t firstHolder = 0;
    std::uint16_t count = 0;
    if (manager == 0 || !TryPointer(manager + kManagerCkptSection, section) ||
        !TryU16(section + kSectionCount, /*highHalf=*/true, count) || count == 0 ||
        !TryPointer(section + kSectionHolderArray, holders) ||
        !TryPointer(holders, firstHolder)) {
        return 0;
    }
    // The checkpoint count and the address of the first holder together: a different course changes
    // one or the other even when the manager itself is reallocated at the same address.
    return (static_cast<std::uint64_t>(count) << 32) | firstHolder;
}

bool ReadCourseRoute(std::vector<RoutePoint>& out) {
    out.clear();

    std::uint32_t manager = CourseToken();
    std::uint32_t section = 0;
    std::uint32_t holders = 0;
    std::uint16_t count = 0;
    if (manager == 0 || !TryPointer(manager + kManagerEnptSection, section) ||
        !TryPointer(section + kSectionHolderArray, holders) ||
        !TryU16(section + kSectionCount, /*highHalf=*/true, count) || count == 0 ||
        count > kMaxCheckpoints) {
        return false;
    }

    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t holder = 0;
        std::uint32_t raw = 0;
        if (!TryPointer(holders + i * kPointerStride, holder) ||
            !TryPointer(holder + kEnptHolderRawEntry, raw)) {
            out.clear();
            return false;
        }
        RoutePoint point;
        if (!TryFloat(raw + kEnptX, point.x) || !TryFloat(raw + kEnptY, point.y) ||
            !TryFloat(raw + kEnptZ, point.z) || !TryFloat(raw + kEnptRange, point.range)) {
            out.clear();
            return false;
        }
        // A point with no successors is legal - it ends a branch - so a failed link read costs
        // that point its links and never the whole route. The count byte is the guest's; the
        // format caps links at 6, so the walk is bounded by both and a corrupt count cannot read
        // past the game's exact-sized array.
        std::uint8_t links = 0;
        std::uint32_t nextArray = 0;
        if (TryU8(holder + kEnptHolderNextCount, links) &&
            TryPointer(holder + kEnptHolderNextArray, nextArray)) {
            if (links > kMaxRouteLinks) {
                links = static_cast<std::uint8_t>(kMaxRouteLinks);
            }
            for (std::uint8_t k = 0; k < links; ++k) {
                std::uint8_t target = 0;
                if (TryU8(nextArray + k, target) && target != kNoRouteLink && target < count) {
                    point.next[point.nextCount++] = target;
                }
            }
        }
        out.push_back(point);
    }

    RT_LOGF(RT_TAG_A11Y, "course route: %u AI route points read\n", static_cast<unsigned>(count));
    return true;
}

bool ReadCourseItemRoute(std::vector<RoutePoint>& out) {
    out.clear();

    std::uint32_t manager = CourseToken();
    std::uint32_t section = 0;
    std::uint32_t holders = 0;
    std::uint16_t count = 0;
    if (manager == 0 || !TryPointer(manager + kManagerItptSection, section) ||
        !TryPointer(section + kSectionHolderArray, holders) ||
        !TryU16(section + kSectionCount, /*highHalf=*/true, count) || count == 0 ||
        count > kMaxCheckpoints) {
        return false;
    }

    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t holder = 0;
        std::uint32_t raw = 0;
        if (!TryPointer(holders + i * kPointerStride, holder) ||
            !TryPointer(holder + kItptHolderRawEntry, raw)) {
            out.clear();
            return false;
        }
        RoutePoint point;
        if (!TryFloat(raw + kEnptX, point.x) || !TryFloat(raw + kEnptY, point.y) ||
            !TryFloat(raw + kEnptZ, point.z) || !TryFloat(raw + kEnptRange, point.range)) {
            out.clear();
            return false;
        }
        // Bounded by the format's 6 inline slots as well as the guest's count byte: past +0x0F
        // sit the prev/next counts themselves, which a corrupt count would read as link indices.
        std::uint8_t links = 0;
        if (TryU8(holder + kItptHolderNextCount, links)) {
            if (links > kMaxRouteLinks) {
                links = static_cast<std::uint8_t>(kMaxRouteLinks);
            }
            for (std::uint8_t k = 0; k < links; ++k) {
                std::uint8_t target = 0;
                if (TryU8(holder + kItptHolderNextInline + k, target) &&
                    target != kNoRouteLink && target < count) {
                    point.next[point.nextCount++] = target;
                }
            }
        }
        out.push_back(point);
    }

    RT_LOGF(RT_TAG_A11Y, "course route: %u item route points read\n",
            static_cast<unsigned>(count));
    return true;
}

bool ReadCourseLapLength(float& out) {
    out = 0.0f;
    std::uint32_t manager = CourseToken();
    std::uint32_t section = 0;
    if (manager == 0 || !TryPointer(manager + kManagerCkptSection, section)) {
        return false;
    }
    return TryFloat(section + kCkptSectionLapLength, out) && out > 0.0f;
}

bool ReadRouteStartPoint(std::uint8_t& out) {
    out = 0;

    std::uint32_t manager = CourseToken();
    std::uint32_t section = 0;
    std::uint32_t holders = 0;
    std::uint32_t holder = 0;
    if (manager == 0 || !TryPointer(manager + kManagerKtptSection, section) ||
        !TryPointer(section + kSectionHolderArray, holders) || !TryPointer(holders, holder)) {
        return false;
    }
    return TryU8(holder + kKtptHolderStartEnpt, out);
}

bool ReadCourseCheckpoints(std::vector<Checkpoint>& out) {
    out.clear();

    std::uint32_t manager = CourseToken();
    if (manager == 0) {
        return false;
    }

    // Gating on the CKPT section rather than on the manager alone is deliberate: the constructor
    // leaves several section slots uninitialised, and Init parses CKPT late, so a non-zero CKPT
    // section is proof that parsing actually finished.
    std::uint32_t section = 0;
    std::uint32_t holders = 0;
    std::uint16_t count = 0;
    if (!TryPointer(manager + kManagerCkptSection, section) ||
        !TryPointer(section + kSectionHolderArray, holders) ||
        !TryU16(section + kSectionCount, /*highHalf=*/true, count) || count == 0 ||
        count > kMaxCheckpoints) {
        return false;
    }

    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t holder = 0;
        std::uint32_t raw = 0;
        if (!TryPointer(holders + i * kPointerStride, holder) ||
            !TryPointer(holder + kHolderRawEntry, raw)) {
            out.clear();
            return false;
        }

        Checkpoint station;
        if (!TryFloat(raw + kCkptLeftX, station.leftX) ||
            !TryFloat(raw + kCkptLeftZ, station.leftZ) ||
            !TryFloat(raw + kCkptRightX, station.rightX) ||
            !TryFloat(raw + kCkptRightZ, station.rightZ)) {
            out.clear();
            return false;
        }
        out.push_back(station);
    }

    RT_LOGF(RT_TAG_A11Y, "course map: %u checkpoints read\n", static_cast<unsigned>(count));
    return true;
}

}  // namespace a11y::race
