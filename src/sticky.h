#pragma once

#include "common.h"

class Editor;

// IntelliJ-style sticky lines: while an editor scrolls, the lines that open what its top line is in (the section, the
// global label, %macro/%if/%rep blocks) stay at the top of the editor, with their line numbers and colors; a click on
// one scrolls to it.
namespace StickyLines {
void Attach(Editor* ed);
}
