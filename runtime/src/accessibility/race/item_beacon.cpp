#include "accessibility/race/item_beacon.h"

#include <algorithm>
#include <cmath>

#include "accessibility/audio/cue_service.h"
#include "accessibility/race/anticipation.h"
#include "accessibility/race/course_map.h"
#include "accessibility/race/guest_read.h"
#include "accessibility/race/heading.h"
#include "accessibility/race/race_state.h"

namespace a11y::race {
namespace {

using audio::CueChannel;
using audio::CueService;
using audio::CueSpec;
using audio::SampleBank;
using audio::SampleId;
using audio::Waveform;

// ObjectsMgr::CreateInstance (0x8082A784) stores the instance here and DestroyInstance zeroes it.
constexpr std::uint32_t kObjectsMgrPtr = 0x809C4330;
constexpr std::uint32_t kMgrObjectCount = 0x18;  // u16, every object on the course
constexpr std::uint32_t kMgrObjectArray = 0x1C;  // Object**
constexpr std::uint32_t kMgrObjectCapacity = 200;  // the array ObjectsMgr::__ct allocates

constexpr std::uint32_t kObjectPosition = 0x30;  // Object::GetPosition (0x80681598) returns this
constexpr std::uint32_t kObjectState = 0xB0;     // Objects::Itembox::IsActive tests this word

// Objects::Itembox::Update flips this back to 1 once the respawn timer elapses; OnCollision sets it
// to 0 when the box is taken. Only 1 is collectable.
constexpr std::uint32_t kItemboxActive = 1;

// Every item box variant - eight of them - shares this one implementation in the same vtable slot,
// so comparing the slot identifies the whole family without listing a single vtable address.
constexpr std::uint32_t kVtableIsActiveSlot = 0xEC;
constexpr std::uint32_t kItemboxIsActiveAddr = 0x806C69C0;  // Objects::Itembox::IsActive

// Item::Manager::CreateInstance (0x80799138) stores the instance here.
constexpr std::uint32_t kItemManagerPtr = 0x809C3618;
constexpr std::uint32_t kItemPlayerArray = 0x14;
constexpr std::uint32_t kItemPlayerStride = 584;
constexpr std::uint32_t kItemPlayerCountPtr = 0x809C38B8;  // u8
constexpr std::uint32_t kItemPlayerHeldItem = 0x8C;

// Item::Player::DecideItem only rolls a new item when the held type reads as this, which is what
// makes it the empty-handed value.
constexpr std::uint32_t kItemNone = 20;

// Racedata::GetPlayerIdOfLocalPlayer, the same chain the race record uses.
constexpr std::uint32_t kRacedataPtr = 0x809BD728;
constexpr std::uint32_t kRacedataLocalToPlayer = 0xB84;

// How far ahead a box is still worth mentioning: the spoken lead at the current speed, the same
// reaction budget every other anticipating cue gets, floored at one road width when stopped.
constexpr float kBeaconRangeSec = kSpokenLeadSec;

constexpr float kBeaconHz = 880.0f;  // A5, clear of the curve beeps and the edge tone
constexpr float kBeaconBlipSec = 0.05f;
constexpr float kBeaconIntervalSec = 0.55f;
constexpr float kBeaconVolumeNear = 0.5f;
constexpr float kBeaconVolumeFar = 0.18f;

// The lean curve below is sin(bearing) * cos(bearing), which peaks at 45 degrees - the bearing that
// has always meant full pan. Dividing by its value there keeps that bearing at exactly full pan.
constexpr float kBeaconLeanPeak = 0.5f;  // sin(pi/4) * cos(pi/4), the peak of the lean curve

// Once the box is behind, the blip drops in pitch instead of vanishing, so passing one is something
// the player hears rather than something that just stops.
constexpr float kBeaconPassedPitch = 0.7f;

// Reads whether the local player is already holding something. Failing to read it is treated as
// empty-handed: a beacon that plays when it should not is a smaller harm than one that never plays.
bool HoldingItem() {
    std::uint32_t racedata = 0;
    std::uint32_t manager = 0;
    std::uint8_t rawId = 0;
    std::uint8_t players = 0;
    if (!TryPointer(kRacedataPtr, racedata) || !TryPointer(kItemManagerPtr, manager) ||
        !TryU8(racedata + kRacedataLocalToPlayer, rawId) ||
        !TryU8(kItemPlayerCountPtr, players)) {
        return false;
    }
    const int id = static_cast<std::int8_t>(rawId);
    if (id < 0 || id >= static_cast<int>(players)) {
        return false;
    }

    std::uint32_t array = 0;
    std::uint32_t held = 0;
    if (!TryPointer(manager + kItemPlayerArray, array) ||
        !Memory::TryRead32(array + static_cast<std::uint32_t>(id) * kItemPlayerStride +
                               kItemPlayerHeldItem,
                           held)) {
        return false;
    }
    return held != kItemNone;
}

bool IsCollectableItembox(std::uint32_t object) {
    std::uint32_t vtable = 0;
    std::uint32_t isActive = 0;
    std::uint32_t state = 0;
    return TryPointer(object, vtable) &&
           Memory::TryRead32(vtable + kVtableIsActiveSlot, isActive) &&
           isActive == kItemboxIsActiveAddr &&
           Memory::TryRead32(object + kObjectState, state) && state == kItemboxActive;
}

// The nearest collectable box within range, as a squared distance so nothing is rooted per object.
bool NearestBox(const RaceState& state, float range, float& outX, float& outZ) {
    std::uint32_t manager = 0;
    std::uint32_t array = 0;
    std::uint16_t count = 0;
    if (!TryPointer(kObjectsMgrPtr, manager) ||
        !TryU16(manager + kMgrObjectCount, /*highHalf=*/true, count) || count == 0 ||
        !TryPointer(manager + kMgrObjectArray, array)) {
        return false;
    }

    const std::uint32_t total = std::min<std::uint32_t>(count, kMgrObjectCapacity);
    const float rangeSq = range * range;
    float bestSq = rangeSq;
    bool found = false;

    for (std::uint32_t i = 0; i < total; ++i) {
        std::uint32_t object = 0;
        if (!TryPointer(array + i * kPointerStride, object) || !IsCollectableItembox(object)) {
            continue;
        }
        float x = 0.0f, y = 0.0f, z = 0.0f;
        if (!TryVec3(object + kObjectPosition, x, y, z)) {
            continue;
        }
        const float dx = x - state.x;
        const float dz = z - state.z;
        const float distanceSq = dx * dx + dz * dz;
        if (distanceSq >= bestSq) {
            continue;
        }
        bestSq = distanceSq;
        outX = x;
        outZ = z;
        found = true;
    }
    return found;
}

// Shared by the live Tick blip and the menu demo, so the two constructions cannot drift apart.
CueSpec MakeBeaconBlip(float pitch, float amplitude, float pan) {
    CueSpec blip;
    if (SampleBank::Instance().Has(SampleId::ItemBox)) {
        blip.sample = SampleId::ItemBox;  // MK64's item box sound, restarted each interval
        blip.pitch = pitch;
    } else {
        blip.shape = Waveform::Sine;
        blip.frequencyHz = kBeaconHz * pitch;
        blip.durationSec = kBeaconBlipSec;
    }
    blip.amplitude = amplitude;
    blip.pan = pan;
    return blip;
}

}  // namespace

void PlayItemBoxCueDemo() {
    CueService::Instance().PlayOneShot(CueChannel::ItemBox,
                                       MakeBeaconBlip(1.0f, kBeaconVolumeNear, 0.0f));
}

void ItemBeacon::Reset() {
    mBlipTimer = 0.0f;
    CueService::Instance().Stop(CueChannel::ItemBox);
}

void ItemBeacon::Tick(const RaceState& state, const CourseMap& map, const Handedness& handedness,
                      int station, float dtSec) {
    if (!state.valid || !state.driving || !map.Loaded() || !handedness.Known()) {
        return;
    }
    // Cheapest test first, and the one that skips the whole object walk most of the time.
    if (HoldingItem()) {
        mBlipTimer = 0.0f;
        return;
    }

    // Before the object walk, not after it: NearestBox is 200 guest reads, and on all but the one
    // frame in an interval where a blip is actually due its result is thrown away.
    mBlipTimer -= dtSec;
    if (mBlipTimer > 0.0f) {
        return;
    }

    const float range =
        std::max(state.speedPerSecond * kBeaconRangeSec, 2.0f * map.MedianHalfWidth());
    float boxX = 0.0f, boxZ = 0.0f;
    if (range <= 0.0f || !NearestBox(state, range, boxX, boxZ)) {
        mBlipTimer = 0.0f;
        return;
    }

    float bearing = 0.0f;
    if (!handedness.BearingTo(state, boxX, boxZ, bearing)) {
        return;
    }
    mBlipTimer = kBeaconIntervalSec;

    const float dx = boxX - state.x;
    const float dz = boxZ - state.z;
    const float distance = std::sqrt(dx * dx + dz * dz);
    const float nearness = std::clamp(1.0f - distance / range, 0.0f, 1.0f);

    // Ahead keeps full pitch; the further behind it falls, the lower it drops.
    const float ahead = std::cos(bearing);
    const float behind = std::clamp(-ahead, 0.0f, 1.0f);
    const float pitch = 1.0f - behind * (1.0f - kBeaconPassedPitch);
    const float amplitude = kBeaconVolumeFar + (kBeaconVolumeNear - kBeaconVolumeFar) * nearness;
    // Pan on sin, not on the bearing itself: a box directly behind reads as a bearing near +/-pi
    // and would slam to a hard side. Folding in the forward component sends it to centre instead,
    // where the dropped pitch is what says "behind".
    const float lean = std::sin(bearing) * std::max(ahead, 0.0f) / kBeaconLeanPeak;
    const float pan = std::clamp(lean, -1.0f, 1.0f);
    CueService::Instance().PlayOneShot(CueChannel::ItemBox, MakeBeaconBlip(pitch, amplitude, pan));
}

}  // namespace a11y::race
