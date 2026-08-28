#ifndef MKW_ACCESSIBILITY_RACE_KCL_OBJECTS_H
#define MKW_ACCESSIBILITY_RACE_KCL_OBJECTS_H

namespace a11y::race {

struct KclPicker;

// The collision meshes that are NOT in course.kcl: the objects that carry their own KCL - Mushroom
// Gorge's caps, logs, the mine carts (docs/kcl-runtime.md §5). Without them the gorge crossing
// probes as a fall everywhere, because the only thing to stand on there is an object.
//
// Read-only, like the rest of the KCL reader: the list and the transforms are parsed out of guest
// memory and the game's own collision queries are never called (§4 - they mutate the controller's
// cached query state, which would change gameplay).
struct KclObjects {
    // Revalidates the manager, its count and each object's controller pointer, rebuilding the
    // cached list when any of them moved, and re-reads every live transform. Call once per frame
    // before probing: a moving object's matrix is only valid for the frame it was read in, and
    // nothing below reads guest memory for it again. False when no object collision is loaded,
    // which is a normal answer on courses that have none.
    static bool Refresh();
    static void Forget();
    static int Count();

    // Offers each object's surface in the world column at (x, z) between `startY` and `stopY` to
    // the picker, converted back to world height, so an object floor competes with the course
    // floor under the picker's own rule and the merge happens in one place. `referenceY` is the
    // height the object's own column is judged against - one surface per object, the one nearest
    // it, which is what an overlay is. The transforms are re-read here rather than cached (§5): a
    // moving object's matrix is only valid for the frame it was read in.
    static void OfferFloors(float x, float z, float startY, float stopY, float referenceY,
                            KclPicker& picker);

    // Objects skipped this refresh, split by cause. Diagnostics only, but the split is what tells
    // a permanent skip apart from a transient one: a null controller means the object had not
    // finished loading (it will come back, and the list keeps retrying), while a header that fails
    // validation means the entry is not an ExternKCL at all - which §5 says is possible and is
    // correctly dropped.
    static int SkippedCount();
    static int SkippedPointerCount();
    static int SkippedHeaderCount();
};

}  // namespace a11y::race

#endif  // MKW_ACCESSIBILITY_RACE_KCL_OBJECTS_H
