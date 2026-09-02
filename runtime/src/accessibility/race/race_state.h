#ifndef MKW_ACCESSIBILITY_RACE_RACE_STATE_H
#define MKW_ACCESSIBILITY_RACE_RACE_STATE_H

#include <cstdint>


namespace a11y::race {

// Everything the race services read, gathered once per frame so no two of them disagree about
// where the kart is. Read-only throughout: nothing here writes back to the game.
struct RaceState {
    // False whenever a race is not loaded, or the kart could not be read this frame. Every service
    // must treat this as "say nothing" rather than as stale data.
    bool valid = false;

    // True only while the player is actually driving - past the countdown, before the finish, and
    // not paused.
    bool driving = false;
    bool finished = false;
    // The game's own race frame counter stopped advancing: the pause menu is up. Timers must not
    // advance and cues must not sound while it is set.
    bool paused = false;
    int countdownFrames = 0;

    float x = 0.0f, y = 0.0f, z = 0.0f;
    float forwardX = 0.0f, forwardZ = 0.0f;  // unit vector in the horizontal plane
    // The game's own forward speed, in units per FRAME - MKW physics has no timestep, so the
    // scalar it multiplies the heading by is per frame, not per second.
    float speed = 0.0f;
    // The same speed converted with the measured frame duration, which is what any threshold
    // expressed in seconds has to divide by. Filled in by the race manager, not read from the game.
    float speedPerSecond = 0.0f;
    // Duration of one guest frame in seconds, from the same TV-format read. Anything differentiated
    // per frame (the yaw estimate) divides by this, never by the host's frame time.
    float frameSec = 0.0f;
    // The game's own 0..1 speed ratio, already capped. Preferred over anything self-calibrated: it
    // is correct from the first frame and is not skewed by a boost or by the engine class.
    float speedRatio = 0.0f;

    int lap = 0;
    int totalLaps = 0;
    int position = 0;
    // The local human's global player id, which is not the same as an index into anything. -1 when
    // it could not be read.
    int playerId = -1;
    // The player's Kart::Player object in guest memory. Lets other services identify the player's
    // kart by pointer instead of by a field offset that would have to be trusted.
    std::uint32_t kartObject = 0;

    // The game's own checkpoint index and lap progress, when they read. -1 means the race record
    // was not available and the caller must fall back to searching the course map.
    int checkpoint = -1;
    float completion = 0.0f;

    std::uint32_t floorFlags = 0;
    // Half the kart body's width across its hitboxes, world units at scale 1: how close to an
    // edge the kart's centre can pass without touching it. Zero when it did not read.
    float bodyHalfWidth = 0.0f;
    // Whether at least one wheel is touching a floor collider this frame. `offRoad` below only
    // updates while this is true - Kart::Movement::UpdateOffroad skips writing the surface
    // multiplier entirely while airborne, so a surface-based cue must gate on this too.
    bool onGround = false;
    // The last surface read while `onGround` was true. Holds its value across airborne frames
    // rather than following whatever `offRoad` would decode to in the air, deliberately: nothing
    // wrote a fresh value there, so decoding it would just be re-reporting the last touchdown.
    bool offRoad = false;
    // The game's own wrong-way flag, not a guess from our course direction. An earlier home-grown
    // version announced continuously while the player was driving correctly.
    bool wrongWay = false;
};

// Reads the player's kart and the race from guest memory. Never throws: a failed read leaves
// `valid` false. Mutable so the caller can fill in the fields that need a frame duration to derive.
RaceState& ReadRaceState();

// Every Kart::Player* in the race - the human and the rivals - for services that act per kart.
// Returns how many were written, 0 when no race is loaded. Lives here so the manager-walk
// constants stay in one file.
int ReadKartObjects(std::uint32_t* out, int maxCount);

// Drops any cached pointers, so the next read re-resolves them. Called when a course unloads.
void ResetRaceState();

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_RACE_STATE_H
