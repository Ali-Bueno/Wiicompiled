#ifndef MKW_ACCESSIBILITY_RACE_CORNER_TECHNIQUE_H
#define MKW_ACCESSIBILITY_RACE_CORNER_TECHNIQUE_H

namespace a11y::race {

struct Curve;

// What a corner asks of the kart at its own top speed, when that is MORE than the corner's grade
// already says. The grades are radii fixed for the reference kart at 150cc (course_curves.cpp):
// gentle and normal hold on the stick, hard needs a drift, a hairpin says slow down. The engine
// class only scales speed (Kart::Movement::__ct, {0.8, 0.9, 1.0}; Retro Rewind 1.5 and 2.4) and
// never the turn rate, so at 200cc a normal corner needs a drift and at 500cc most hard ones need
// the brakes; the geometry word stays, the technique word follows the class.
enum class CornerTechnique { None, Drift, Brake };

// `baseSpeed` is RaceState::baseSpeed, units per frame; zero or unread gives None.
CornerTechnique ExtraTechnique(const Curve& curve, float baseSpeed);

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_CORNER_TECHNIQUE_H
