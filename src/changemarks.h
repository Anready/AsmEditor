#pragma once

#include "codeinfo.h"
#include "common.h"

class Editor;

// The change margin, as in IntelliJ: each run of added / changed lines is one bar with rounded ends, a deleted
// place a short rounded mark at the top of the line after it. Drawn by Scintilla itself (image markers per piece of
// a run: top, middle, bottom, single), so it scrolls with the text without flicker.
namespace ChangeMarks {
void Attach(Editor* ed);
// Marks the changes: Editor::kMarkerAdded/Modified/Deleted record them (the scrollbar shows those), the image
// markers draw them.
void Show(Editor& ed, const std::map<int, LineDiff::Change>& changes);
}
