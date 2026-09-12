#ifndef MKW_ACCESSIBILITY_MII_MII_LICENSE_PAGE_H
#define MKW_ACCESSIBILITY_MII_MII_LICENSE_PAGE_H

namespace a11y::mii {

// Repaints the licence buttons of Pages::LicenseSelect when it is the page on screen, so a name
// changed while it is open shows at once instead of on the next visit. Runs the page's own two
// calls per valid licence (docs/mii-name.md, "Repainting the licence screen"). False when another
// page is on top; that page rebuilds itself on entry.
bool RepaintLicenseSelect();

}  // namespace a11y::mii

#endif  // MKW_ACCESSIBILITY_MII_MII_LICENSE_PAGE_H
