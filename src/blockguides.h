#pragma once

#include "common.h"

class Editor;

// IntelliJ-style block guides: a thin vertical line under each block that opens on a line (global label, %macro,
// %if, %rep) down to its end, brighter for the block the caret is in; the fold margin highlights that block too.
namespace BlockGuides {
void Attach(Editor* ed);
}
