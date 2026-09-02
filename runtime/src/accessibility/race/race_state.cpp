#include "accessibility/race/race_state.h"

#include <algorithm>
#include <cmath>

#include "accessibility/race/guest_read.h"
#include "accessibility/race/race_record.h"

namespace a11y::race {
namespace {

// Every address and offset below was recovered from the translated body of the function named
// beside it, never guessed. Nothing here is hooked: none of the kart update functions are
// interceptable - the translator short-circuits their call sites with MKW_TRANSLATED_TRAIT, so a
// registration would compile, link and register but never run. Reading the singleton chain from
// the frame tick is both the robust option and the one that needs no retranslation.

// Kart::Manager::CreateInstance (0x8058FAA8) stores the instance here; DestroyInstance (0x8058FAF8)
// writes zero back, so a null check is a sound "no race loaded" test.
constexpr std::uint32_t kKartManagerPtr = 0x809C18F8;
constexpr std::uint32_t kManagerKartArray = 0x20;  // Kart::Manager::GetKartPlayer (0x80590100)
constexpr std::uint32_t kManagerKartCount = 0x24;  // loop bound in Kart::Manager::Update/Init

// Kart::Link holds a table of sub-object pointers at +0x00; every Kart::Link::Get* reads it.
constexpr std::uint32_t kAccStatus = 0x04;    // Kart::Link::IsCPU / IsLocal
constexpr std::uint32_t kAccBody = 0x08;      // Kart::Link::GetBody
constexpr std::uint32_t kAccMovement = 0x28;  // Kart::Link::GetMovement

constexpr std::uint32_t kStatusTypeFlags = 0x14;  // Kart::Link::IsLocal reads bit 1 of this word
constexpr std::uint32_t kStatusLocalBit = 1u << 1;

constexpr std::uint32_t kBodyPhysicsHolder = 0x90;  // Kart::Link::GetPhysicsHolder (0x805903AC)
constexpr std::uint32_t kHolderPhysics = 0x04;      // Kart::Link::GetPhysics (0x805903CC)
constexpr std::uint32_t kHolderHitboxGroup = 0x08;  // body hitbox group
constexpr std::uint32_t kHitboxKclFlags = 0x74;     // Kart::Link::GetBodyClosestFloorFlags
// HitboxGroup, from HitboxGroup::UpdateBoundingRadius (0x805B883C): the hitbox count at +0 and
// the Hitbox array at +0x8C, each 0x30 bytes holding its BSP::Hitbox* at +0. The BSP hitbox has
// `enable` at +0, position at +4 and radius at +0x10 (HitboxGroup::__ct(hitboxes), 0x805B84C0).
// The group's own +4 is the bounding radius over ALL axes - a kart's half-length - so the
// x-axis pass is repeated here for the width alone.
constexpr std::uint32_t kHitboxGroupCount = 0x00;    // s16
constexpr std::uint32_t kHitboxGroupHitboxes = 0x8C; // Hitbox*
constexpr std::uint32_t kHitboxStride = 0x30;
constexpr std::uint32_t kHitboxBsp = 0x00;           // BSP::Hitbox*
constexpr std::uint32_t kBspHitboxEnable = 0x00;     // u16
constexpr std::uint32_t kBspHitboxPositionX = 0x04;  // Vec3 x
constexpr std::uint32_t kBspHitboxRadius = 0x10;
constexpr std::uint32_t kHolderBodyMatrix = 0x9C;   // Kart::Link::GetMtx (0x80590264), Mtx34

constexpr std::uint32_t kPhysicsPosition = 0x68;  // Kart::Link::SetKartPosition (0x80590238)

constexpr std::uint32_t kMovementSpeed = 0x20;  // Kart::Link::GetEngineSpeed (0x80590CF8)
// The axis the scalar speed is applied along: Movement::Update computes velocity as speed times
// this vector, so it is the kart's forward direction and the sign of travel lives in the speed.
constexpr std::uint32_t kMovementHeading = 0x74;
constexpr std::uint32_t kMovementSpeedRatio = 0xB0;  // Kart::Link::GetSpeedRatioCapped (0x80590DC0)
constexpr std::uint32_t kMovementOffroad = 0xB8;     // Kart::Movement::UpdateOffroad (0x8057C3D4)
// u16, same function: floorCollisionCount, the wheel-contact count UpdateOffroad checks before it
// touches kMovementOffroad at all - with none in contact it returns early and never writes it.
constexpr std::uint32_t kMovementFloorCollisionCount = 0xC8;

// The body matrix is row-major 3x4, so a column is three floats a row apart.
constexpr std::uint32_t kMatrixRowBytes = 16;
constexpr std::uint32_t kMatrixForwardColumn = 2;

// UpdateOffroad writes a speed multiplier here and forces it to exactly 1.0 while the kart is
// immune (star, mega, bullet). Anything under 1 is the surface slowing the kart down, which is a
// better off-road test than decoding KCL flags: it is already normalised across surface types and
// kart stats, and it needs no immunity special case.
constexpr float kOnRoadMultiplier = 1.0f;

// Two unit headings state the same axis when they agree within 45 degrees: the body matrix's
// columns are a right angle apart, so half of that is a boundary no neighbouring axis can cross.
constexpr float kHeadingAgreeDot = 0.70710678f;  // cos(45 degrees)

RaceState g_state;

// Everything below survives across frames on purpose and is cleared by ResetRaceState, which a
// function-local static would outlive - handing the next race the previous one's cached answer.

// ReadRaceState resets g_state to RaceState{} on every call, so the last-known-on-ground surface
// has nowhere else to live while the kart is airborne.
bool g_lastGroundOffRoad = false;

// The body hitbox walk is fixed for the race, so it is cached per hitbox group.
std::uint32_t g_hitboxGroup = 0;
float g_bodyHalfWidth = 0.0f;

// Whether the body matrix's column 2 really is the forward axis - checked against the velocity
// heading while the kart moves - and the last heading it had while moving. Both stand in at a
// standstill, where the velocity heading has gone stale.
enum class ColumnCheck { Unchecked, Forward, NotForward };
ColumnCheck g_columnCheck = ColumnCheck::Unchecked;
float g_movingForwardX = 0.0f;
float g_movingForwardZ = 0.0f;

// One entry of Kart::Manager's array, with its sub-object table.
bool ResolveKart(std::uint32_t karts, std::uint32_t index, std::uint32_t& kartOut,
                 std::uint32_t& accOut) {
    return TryPointer(karts + index * kPointerStride, kartOut) && TryPointer(kartOut, accOut);
}

// Kart::Link::IsLocal: bit 1 of the status word.
bool IsLocalKart(std::uint32_t acc) {
    std::uint32_t status = 0;
    std::uint32_t flags = 0;
    return TryPointer(acc + kAccStatus, status) &&
           Memory::TryRead32(status + kStatusTypeFlags, flags) && (flags & kStatusLocalBit) != 0;
}

// The kart the human is driving. Indexed by the player id the race record already resolved through
// Racedata, so the mod has ONE definition of "the player" instead of two that can disagree; the
// local-bit scan is the fallback for the frames where the record has not read yet. Never cached: a
// cached index would go stale across a restart in a way that is silent and hard to notice.
bool FindPlayerKart(std::uint32_t manager, int playerId, std::uint32_t& kartOut,
                    std::uint32_t& accOut) {
    std::uint32_t karts = 0;
    std::uint8_t count = 0;
    if (!TryPointer(manager + kManagerKartArray, karts) ||
        !TryU8(manager + kManagerKartCount, count)) {
        return false;
    }

    // Confirmed against the same local bit the scan uses, so the two answers can never diverge.
    if (playerId >= 0 && playerId < static_cast<int>(count) &&
        ResolveKart(karts, static_cast<std::uint32_t>(playerId), kartOut, accOut) &&
        IsLocalKart(accOut)) {
        return true;
    }

    for (std::uint32_t i = 0; i < count; ++i) {
        if (ResolveKart(karts, i, kartOut, accOut) && IsLocalKart(accOut)) {
            return true;
        }
    }
    kartOut = 0;
    accOut = 0;
    return false;
}

// Normalises a horizontal vector in place; false when it states no direction.
bool Normalize2(float& x, float& z) {
    const float len = std::sqrt(x * x + z * z);
    if (len <= 0.0f) {
        return false;
    }
    x /= len;
    z /= len;
    return true;
}

// Column 2 of the body matrix, as a horizontal unit vector.
bool ReadForwardColumn(std::uint32_t holder, float& x, float& z) {
    const std::uint32_t column = holder + kHolderBodyMatrix + kMatrixForwardColumn * 4;
    return TryFloat(column, x) && TryFloat(column + 2 * kMatrixRowBytes, z) && Normalize2(x, z);
}

// Which way the kart is pointing. Preferred over a matrix column because it needs no assumption
// about which local axis is forward - the game multiplies this vector by the scalar speed to get
// velocity, so it is the forward axis by construction. It goes stale at a standstill, and the
// steering guide is the player's recovery signal exactly there, so the column only stands in once
// it has been shown to agree with this vector while the kart was moving.
bool ReadHeading(std::uint32_t movement, std::uint32_t holder, float speed, float& fx, float& fz) {
    float tx = 0.0f, ty = 0.0f, tz = 0.0f;
    float mx = 0.0f, mz = 0.0f;
    if (std::fabs(speed) > 0.0f && TryVec3(movement + kMovementHeading, tx, ty, tz) &&
        Normalize2(tx, tz)) {
        // Settled here and once: this is the only moment the game's own forward axis is live to
        // check the column against.
        if (g_columnCheck == ColumnCheck::Unchecked && ReadForwardColumn(holder, mx, mz)) {
            g_columnCheck = (tx * mx + tz * mz) >= kHeadingAgreeDot ? ColumnCheck::Forward
                                                                    : ColumnCheck::NotForward;
        }
        fx = tx;
        fz = tz;
        g_movingForwardX = tx;
        g_movingForwardZ = tz;
        return true;
    }

    if (g_columnCheck == ColumnCheck::Forward && ReadForwardColumn(holder, mx, mz)) {
        fx = mx;
        fz = mz;
        return true;
    }
    if (g_movingForwardX != 0.0f || g_movingForwardZ != 0.0f) {
        fx = g_movingForwardX;  // a rejected column: hold the last heading the kart really had
        fz = g_movingForwardZ;
        return true;
    }
    // Nothing has moved yet - the start line - so the unchecked column is all there is, and
    // without it the whole state would read as invalid through the countdown.
    return g_columnCheck == ColumnCheck::Unchecked && ReadForwardColumn(holder, fx, fz);
}

// The x extent of the body's hitbox spheres, the same pass UpdateBoundingRadius makes per axis.
// Fixed for the race, so it is only walked once per kart.
float ReadBodyHalfWidth(std::uint32_t hitboxGroup) {
    if (hitboxGroup == g_hitboxGroup && g_bodyHalfWidth > 0.0f) {
        return g_bodyHalfWidth;
    }
    std::uint16_t count = 0;
    std::uint32_t hitboxes = 0;
    if (!TryU16(hitboxGroup + kHitboxGroupCount, /*highHalf=*/true, count) ||
        !TryPointer(hitboxGroup + kHitboxGroupHitboxes, hitboxes)) {
        return 0.0f;
    }
    float halfWidth = 0.0f;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t bsp = 0;
        std::uint16_t enabled = 0;
        float x = 0.0f, radius = 0.0f;
        if (!TryPointer(hitboxes + i * kHitboxStride + kHitboxBsp, bsp) ||
            !TryU16(bsp + kBspHitboxEnable, /*highHalf=*/true, enabled) || enabled == 0 ||
            !TryFloat(bsp + kBspHitboxPositionX, x) || !TryFloat(bsp + kBspHitboxRadius, radius)) {
            continue;
        }
        halfWidth = std::max(halfWidth, radius + std::fabs(x));
    }
    g_hitboxGroup = hitboxGroup;
    g_bodyHalfWidth = halfWidth;
    return halfWidth;
}

}  // namespace

int ReadKartObjects(std::uint32_t* out, int maxCount) {
    std::uint32_t manager = 0;
    std::uint32_t karts = 0;
    std::uint8_t count = 0;
    if (!TryPointer(kKartManagerPtr, manager) ||
        !TryPointer(manager + kManagerKartArray, karts) ||
        !TryU8(manager + kManagerKartCount, count)) {
        return 0;
    }
    int written = 0;
    for (std::uint32_t i = 0; i < count && written < maxCount; ++i) {
        std::uint32_t kart = 0;
        if (TryPointer(karts + i * kPointerStride, kart)) {
            out[written++] = kart;
        }
    }
    return written;
}

void ResetRaceState() {
    g_state = RaceState{};
    g_lastGroundOffRoad = false;
    g_hitboxGroup = 0;
    g_bodyHalfWidth = 0.0f;
    g_columnCheck = ColumnCheck::Unchecked;
    g_movingForwardX = 0.0f;
    g_movingForwardZ = 0.0f;
    ResetRaceRecord();
}

RaceState& ReadRaceState() {
    g_state = RaceState{};

    // The record first, because it settles who the player is; the kart is then looked up by that
    // same id. Its fields are blanked again below if no kart stands behind them, so `valid` keeps
    // meaning "everything in here was read this frame".
    FillRaceRecord(g_state);

    std::uint32_t manager = 0;
    if (!TryPointer(kKartManagerPtr, manager)) {
        g_state = RaceState{};  // no race loaded
        return g_state;
    }

    std::uint32_t acc = 0;
    if (!FindPlayerKart(manager, g_state.playerId, g_state.kartObject, acc)) {
        g_state = RaceState{};
        return g_state;
    }

    std::uint32_t body = 0, holder = 0, physics = 0, movement = 0;
    if (!TryPointer(acc + kAccBody, body) || !TryPointer(body + kBodyPhysicsHolder, holder) ||
        !TryPointer(holder + kHolderPhysics, physics) ||
        !TryPointer(acc + kAccMovement, movement)) {
        g_state = RaceState{};
        return g_state;
    }

    if (!TryVec3(physics + kPhysicsPosition, g_state.x, g_state.y, g_state.z) ||
        !TryFloat(movement + kMovementSpeed, g_state.speed) ||
        !ReadHeading(movement, holder, g_state.speed, g_state.forwardX, g_state.forwardZ)) {
        g_state = RaceState{};
        return g_state;
    }

    TryFloat(movement + kMovementSpeedRatio, g_state.speedRatio);

    std::uint16_t floorCollisionCount = 0;
    g_state.onGround = TryU16(movement + kMovementFloorCollisionCount, /*highHalf=*/true,
                              floorCollisionCount) &&
                       floorCollisionCount > 0;

    // Only decode the surface multiplier while grounded: UpdateOffroad leaves it untouched in the
    // air, so decoding it there would just replay the last surface the kart actually touched.
    if (g_state.onGround) {
        float offroad = kOnRoadMultiplier;
        if (TryFloat(movement + kMovementOffroad, offroad)) {
            g_lastGroundOffRoad = offroad < kOnRoadMultiplier;
        }
    }
    g_state.offRoad = g_lastGroundOffRoad;

    std::uint32_t hitboxGroup = 0;
    if (TryPointer(holder + kHolderHitboxGroup, hitboxGroup)) {
        Memory::TryRead32(hitboxGroup + kHitboxKclFlags, g_state.floorFlags);
        g_state.bodyHalfWidth = ReadBodyHalfWidth(hitboxGroup);
    }

    g_state.valid = true;
    return g_state;
}

}  // namespace a11y::race
