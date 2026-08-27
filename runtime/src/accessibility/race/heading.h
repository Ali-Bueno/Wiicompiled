#ifndef MKW_ACCESSIBILITY_RACE_HEADING_H
#define MKW_ACCESSIBILITY_RACE_HEADING_H

namespace a11y::race {

struct RaceState;
class CourseMap;

// One direction convention, for the whole mod.
//
// Which of the two perpendiculars of a forward vector counts as "right" is a property of the
// world's coordinate system, and the checkpoint's own left-to-right vector already settled it
// when the course map was built. The kart lives in the same world, so its right is the same
// perpendicular - derived from track data alone, with nothing observed or calibrated while
// driving.
//
// Living in one place is the point. In the MK64 mod a guessed sign convention, applied
// inconsistently between speech and panning, was the single most repeated bug; here every signed
// value the player can hear comes through this one object.
class Handedness {
public:
    // Call once per frame before anything asks for a bearing.
    void Observe(const RaceState& state, const CourseMap& map, int station);

    // On course change: the sign re-derives from the next course's map, and a cue firing in the
    // gap between maps must stay silent rather than borrow the previous course's vote.
    void Forget() { mKnown = false; }

    bool Known() const { return mKnown; }

    // Signed bearing from the kart to a point, in radians, positive when the point lies to the
    // kart's right. False until the convention has been established.
    bool BearingTo(const RaceState& state, float targetX, float targetZ, float& bearingOut) const;

    // The kart's right-hand unit vector. The ONE place that derivation lives - every cue that
    // needs "the kart's right" takes it from here, so no module can drift onto its own sign.
    void RightVector(const RaceState& state, float& x, float& z) const;

private:
    bool mKnown = false;
    bool mRightIsFzNegFx = true;
};

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_HEADING_H
