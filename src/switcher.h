#pragma once

#include "common.h"

// IntelliJ-style switcher (Ctrl+Tab): a list of the open files in the order they were used. While Ctrl is held, Tab
// and Shift+Tab (or the arrows) move through it; releasing Ctrl, Enter or a click picks, Esc cancels.
namespace Switcher {
struct Item {
    std::wstring name;
    std::wstring detail;  // dimmed, after the name (the folder)
    int icon = -1;        // FileIcons::Kind
};
std::optional<int> Run(HWND owner, const std::wstring& title, const std::vector<Item>& items, int start);
}
