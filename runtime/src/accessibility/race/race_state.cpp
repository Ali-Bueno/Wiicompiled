#include "accessibility/race/race_state.h"

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
constexpr std::uint32_t kHolderBodyMatrix = 0x9C;   // Kart::Link::GetMtx (0x80590264), Mtx34

constexpr std::uint32_t kPhysicsPosition = 0x68;  // Kart::Link::SetKartPosition (0x80590238)

constexpr std::uint32_t kMovementSpeed = 0x20;  // Kart::Link::GetEngineSpeed (0x80590CF8)
// The axis the scalar speed is applied along: Movement::Update computes velocity as speed times
// this vector, so it is the kart's forward direction and the sign of travel lives in the speed.
constexpr std::uint32_t kMovementHeading = 0x74;
constexpr std::uint32_t kMovementSpeedRatio = 0xB0;  // Kart::Link::GetSpeedRatioCapped (0x80590DC0)
constexpr std::uint32_t kMovementOffroad = 0xB8;     // Kart::Movement::UpdateOffroad (0x8057C3D4)

// The body matrix is row-major 3x4, so a column is three floats a row apart.
constexpr std::uint32_t kMatrixRowBytes = 16;
constexpr std::uint32_t kMatrixForwardColumn = 2;

// UpdateOffroad writes a speed multiplier here and forces it to exactly 1.0 while the kart is
// immune (star, mega, bullet). Anything under 1 is the surface slowing the kart down, which is a
// better off-road test than decoding KCL flags: it is already normalised across surface types and
// kart stats, and it needs no immunity special case.
constexpr float kOnRoadMultiplier = 1.0f;

RaceState g_state;

// The kart the human is driving. Scanned every frame rather than cached: the count is small, and a
// cached index would go stale across a restart in a way that is silent and hard to notice.
bool FindLocalKart(std::uint32_t manager, std::uint32_t& kartOut, std::uint32_t& accOut) {
    std::uint32_t karts = 0;
    std::uint8_t count = 0;
    if (!TryPointer(manager + kManagerKartArray, karts) ||
        !TryU8(manager + kManagerKartCount, count)) {
        return false;
    }

    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t kart = 0;
        std::uint32_t acc = 0;
        std::uint32_t status = 0;
        std::uint32_t flags = 0;
        if (!TryPointer(karts + i * kPointerStride, kart) || !TryPointer(kart, acc) ||
            !TryPointer(acc + kAccStatus, status) ||
            !Memory::TryRead32(status + kStatusTypeFlags, flags)) {
            continue;
        }
        if ((flags & kStatusLocalBit) != 0) {
            kartOut = kart;
            accOut = acc;
            return true;
        }
    }
    return false;
}

// Which way the kart is pointing. Preferred over a matrix column because it needs no assumption
// about which local axis is forward - the game multiplies this vector by the scalar speed to get
// velocity, so it is the forward axis by construction. It goes stale at a standstill, which is the
// only case where the body matrix has to stand in.
bool ReadHeading(std::uint32_t movement, std::uint32_t holder, float speed, float& fx, float& fz) {
    float tx = 0.0f, ty = 0.0f, tz = 0.0f;
    if (std::fabs(speed) > 0.0f && TryVec3(movement + kMovementHeading, tx, ty, tz)) {
        const float len = std::sqrt(tx * tx + tz * tz);
        if (len > 0.0f) {
            fx = tx / len;
            fz = tz / len;
            return true;
        }
    }

    // Column 2 of the body matrix. Which column is forward is a Wii convention the translation does
    // not state, so this is only a fallback while stopped, where being wrong changes nothing the
    // player can hear.
    const std::uint32_t column = holder + kHolderBodyMatrix + kMatrixForwardColumn * 4;
    float mx = 0.0f, mz = 0.0f;
    if (!TryFloat(column, mx) || !TryFloat(column + 2 * kMatrixRowBytes, mz)) {
        return false;
    }
    const float len = std::sqrt(mx * mx + mz * mz);
    if (len <= 0.0f) {
        return false;
    }
    fx = mx / len;
    fz = mz / len;
    return true;
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
}

RaceState& ReadRaceState() {
    g_state = RaceState{};

    std::uint32_t manager = 0;
    if (!TryPointer(kKartManagerPtr, manager)) {
        return g_state;  // no race loaded
    }

    std::uint32_t acc = 0;
    if (!FindLocalKart(manager, g_state.kartObject, acc)) {
        return g_state;
    }

    std::uint32_t body = 0, holder = 0, physics = 0, movement = 0;
    if (!TryPointer(acc + kAccBody, body) || !TryPointer(body + kBodyPhysicsHolder, holder) ||
        !TryPointer(holder + kHolderPhysics, physics) ||
        !TryPointer(acc + kAccMovement, movement)) {
        return g_state;
    }

    if (!TryVec3(physics + kPhysicsPosition, g_state.x, g_state.y, g_state.z) ||
        !TryFloat(movement + kMovementSpeed, g_state.speed) ||
        !ReadHeading(movement, holder, g_state.speed, g_state.forwardX, g_state.forwardZ)) {
        return g_state;
    }

    TryFloat(movement + kMovementSpeedRatio, g_state.speedRatio);

    float offroad = kOnRoadMultiplier;
    if (TryFloat(movement + kMovementOffroad, offroad)) {
        g_state.offRoad = offroad < kOnRoadMultiplier;
    }

    std::uint32_t hitboxGroup = 0;
    if (TryPointer(holder + kHolderHitboxGroup, hitboxGroup)) {
        Memory::TryRead32(hitboxGroup + kHitboxKclFlags, g_state.floorFlags);
    }

    // Sets `driving`, which stays false unless the race stage reads back as racing. Deliberately
    // conservative: without confirming the race is live it is better to say nothing than to play
    // cues over an intro camera or a finished race.
    FillRaceRecord(g_state);

    g_state.valid = true;
    return g_state;
}

}  // namespace a11y::race
