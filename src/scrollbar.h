#pragma once

#include "common.h"

// IntelliJ-style scrollbars: a rounded thumb on a strip of the window's own background, drawn by small child windows
// over its right (and bottom) edge instead of the system scrollbars, and smooth wheel scrolling (a notch scrolls in a
// few steps instead of jumping). Works for Scintilla and for system controls with WS_VSCROLL (tree views, list boxes,
// multi-line edits); those keep their own scrollbar, moved out of sight: the control is made wider by its width and
// clipped back to the size it was given.
namespace ScrollBars {
void Attach(HWND hwnd);
// For a window that is still being created (from a CBT hook): attached as soon as the message loop runs.
void AttachLater(HWND hwnd);
// Which bars a Scintilla window shows (both by default): the main editor's vertical one is its mark bar, the
// debugger's views do not scroll sideways.
void Show(HWND hwnd, bool vertical, bool horizontal);
// The width of the vertical bar over the right edge of the window while it is shown, else 0.
int VerticalWidth(HWND hwnd);
// Something the bars show may have changed out of sight (text added below the screen): they update a moment later.
void Changed(HWND hwnd);
}
