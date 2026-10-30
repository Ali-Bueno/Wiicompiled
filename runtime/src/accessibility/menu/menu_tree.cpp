#include "accessibility/menu/menu_tree.h"

namespace a11y::menu {

void MenuCursor::Reset(const std::vector<MenuRow>* root) {
    mLevels.clear();
    if (root != nullptr && !root->empty()) {
        mLevels.push_back({root, 0});
    }
}

const MenuRow* MenuCursor::Focused() const {
    if (mLevels.empty()) {
        return nullptr;
    }
    const Level& level = mLevels.back();
    return &(*level.rows)[static_cast<size_t>(level.focus)];
}

const MenuRow* MenuCursor::Parent() const {
    if (mLevels.size() < 2) {
        return nullptr;
    }
    const Level& level = mLevels[mLevels.size() - 2];
    return &(*level.rows)[static_cast<size_t>(level.focus)];
}

void MenuCursor::Move(int delta) {
    if (mLevels.empty()) {
        return;
    }
    Level& level = mLevels.back();
    const int count = static_cast<int>(level.rows->size());
    level.focus = ((level.focus + delta) % count + count) % count;
}

bool MenuCursor::Enter() {
    const MenuRow* row = Focused();
    if (row == nullptr || !row->IsSubmenu()) {
        return false;
    }
    mLevels.push_back({&row->children, 0});
    return true;
}

bool MenuCursor::Leave() {
    if (mLevels.size() < 2) {
        return false;
    }
    mLevels.pop_back();
    return true;
}

}  // namespace a11y::menu
