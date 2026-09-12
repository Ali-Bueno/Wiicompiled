#ifndef MKW_ACCESSIBILITY_MII_MII_IDENTITY_H
#define MKW_ACCESSIBILITY_MII_MII_IDENTITY_H

// The player's Mii: the port has no Mii Channel, so the game's licence is one of RFL's six
// built-in "no name" Miis, and every console prints "Player" for those instead of a name. This
// puts a Mii of our own in the Mii database, named from `[system] mii_name` with a look and a
// createID derived from that name, and moves the licence onto it. Design in docs/mii-name.md.
namespace a11y::mii {

// Stamps the default Mii table once, before the game reads it. Empty name = nothing changes.
void Init();

// Re-reads the configured name and stamps it again; the database record and the licence move
// to the new id on the next Tick, the same way an earlier name's id is moved.
void Refresh();

// Keeps the database record in place and the selected licence (and its save block) on our Mii.
void Tick();

}  // namespace a11y::mii

#endif  // MKW_ACCESSIBILITY_MII_MII_IDENTITY_H
