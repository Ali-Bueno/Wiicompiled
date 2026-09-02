#ifndef MKW_ACCESSIBILITY_RACE_ENGINE_PAN_H
#define MKW_ACCESSIBILITY_RACE_ENGINE_PAN_H

namespace a11y::race {

struct RaceState;

// Steers the player's own engine note left and right, and does nothing else.
//
// This is the steering guide. It adds no sound: Mario Kart Wii's engine already rises with speed,
// exactly as a sighted player hears it, so the only thing missing for a blind player is *where the
// track goes*. Replacing the engine with a synthesized tone would put a second engine on top of the
// first, which is not an accessibility cue - it is noise. Volume and pitch are never touched.
//
// The one thing this writes to guest memory is a pan value on one sound object. It changes nothing
// mechanical: no physics, no timing, no odds. A sighted player hears their engine positioned by the
// camera; a blind player hears it positioned by the racing line.
class EnginePan {
public:
    void Reset();

    // `pan` is -1 hard left, 0 centred, +1 hard right. Silently does nothing when the engine sound
    // is not currently playing, which is normal - the sound comes from a pool and is restarted.
    // With `active` false the engine is handed back to the game: the external term is zeroed -
    // retried until that write lands - and the camera-relative placement resumes, instead of being
    // cancelled into a forced centre.
    void Apply(const RaceState& state, float pan, bool active);

private:
    bool mAnnouncedSlot = false;
    bool mIdleRestored = false;
    // Temporary diagnostic: which link of the chain the last attempt reached. Remove with the log
    // in the .cpp once panning is confirmed on a real run.
    int mLastStage = 0;
};

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_ENGINE_PAN_H
