#pragma once

#include "common.h"

// IntelliJ-style file icons by extension (assembly, include, C/C++, Python, text, config, scripts, binaries…), drawn
// once per size with anti-aliased edges. Used by the project tree, the tabs and the file lists.
namespace FileIcons {
enum class Kind {
    Folder, FolderOpen, File, Asm, Include, C, Cpp, Header, Python, Text, Markdown, Config, Script, Exe, Binary,
    Archive, Image, Web, JavaScript,
    Function, Macro, Section, Command,  // not files: symbols and commands in the search lists
    Count
};
Kind KindOf(const fs::path& path, bool folder = false, bool open = false);
// Draws the icon (size x size pixels) with its top left corner at x, y.
void Draw(HDC dc, int x, int y, int size, Kind kind);
// All the icons in the order of Kind, for a tree view (kept for the life of the program).
HIMAGELIST ImageList(int size);
}
