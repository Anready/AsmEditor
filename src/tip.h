#pragma once

#include "common.h"

// The hint card over the editor (instruction reference, values while debugging, problems, messages), in the
// style of IntelliJ's quick documentation: a rounded card with a shadow above the line (below it when there is no
// room), the header (bytes [hltFrom, hltTo) of the text) in the code font and the link color under a separator,
// prose in the interface font, aligned text (tables of values) in the code font. One card at a time.
namespace Tip {

// x: where the hinted text starts; lineTop/lineBottom: its line (screen coordinates).
void Show(HWND editor, int x, int lineTop, int lineBottom, const std::string& utf8, size_t hltFrom = 0,
          size_t hltTo = 0);
void Hide();
void HideUnlessNew();  // when the caret moved or the text scrolled: not a card just shown for that move (a jump)
bool Visible();
HWND Handle();  // nullptr before the first card

}  // namespace Tip
