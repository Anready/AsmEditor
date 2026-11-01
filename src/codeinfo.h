#pragma once

#include "common.h"

// What the editor shows about the code beside the text (no UI here; covered by the unit tests).

namespace Inlay {
// Inlay hints, IntelliJ-style, at the end of lines (0-based line -> text): which parameter of a called function a
// register gets ("format → printf"), the prototype on the call line, and the size of a memory operand that the
// other operand implies ("mov [x], eax" accesses a dword).
std::map<int, std::string> Hints(const std::string& text);
// The general-purpose and vector registers named on a line (lower case, once each, in order; not in comments).
std::vector<std::string> Registers(const std::string& line);
}

namespace LineDiff {
enum class Change { Added, Modified, Deleted };  // Deleted: lines were removed just above this one
// How `now` differs from `base`, by line (0-based lines of `now`). Lines are compared without trailing "\r".
std::map<int, Change> Compare(const std::vector<std::string>& base, const std::vector<std::string>& now);
std::vector<std::string> SplitLines(const std::string& text);
}
