#ifndef MKW_ACCESSIBILITY_MENU_MENU_TREE_H
#define MKW_ACCESSIBILITY_MENU_MENU_TREE_H

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace a11y::menu {

// A row is a value (name + spoken value + adjust), an action (name + activate), both for
// switches, or a submenu (name + children, entered with Activate).
struct MenuRow {
    std::string nameKey;
    std::function<std::string()> value;
    std::function<void(int)> adjust;    // -1 left, +1 right
    std::function<void()> activate;
    std::vector<MenuRow> children;

    bool IsSubmenu() const { return !children.empty(); }
};

// Where the cursor is in a row tree: one focus per open level, so leaving a submenu lands back
// on the row that opened it. The tree must outlive the cursor and never change while it is used.
class MenuCursor {
public:
    void Reset(const std::vector<MenuRow>* root);

    // Null when the tree is empty.
    const MenuRow* Focused() const;
    // The submenu row the current level belongs to; null at the top.
    const MenuRow* Parent() const;
    size_t Depth() const { return mLevels.size(); }

    // Wraps within the current level.
    void Move(int delta);
    // Into the focused submenu, on its first row. False when the focused row is not one.
    bool Enter();
    // Back to the parent's row. False at the top.
    bool Leave();

private:
    struct Level {
        const std::vector<MenuRow>* rows;
        int focus;
    };
    std::vector<Level> mLevels;
};

}  // namespace a11y::menu

#endif  // MKW_ACCESSIBILITY_MENU_MENU_TREE_H
