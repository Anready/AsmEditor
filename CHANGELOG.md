# Changelog

The release workflow publishes the section of the tagged version as the release notes.

## 1.3.0

Windows and tabs as in IntelliJ IDEA, for one monitor or two.

- Every window is made of one or two tab groups, side by side or one above the other, and each group has a tab row
  of its own with as many files as you like: the editor, the debug panel and windows of their own.
- Drag a tab along its row to reorder it, onto another group to move it there, onto the right or lower third of an
  editor to split it, or out of every window to give it a window of its own (e.g. on a second monitor). The
  translucent highlight shows where it goes.
- Drag a window of its own by its title onto the main window to dock its tabs back; closing it puts the files back
  in the editor and the debugger's tabs back in the debug panel. Right-click a tab for the same commands.
- The debugger's views (registers, call stack, watches, code, flags, memory) are tabs like files: put them in any
  group or window, next to files, and split them. Where they are is remembered. View → Tool windows moves the
  whole debug panel into a window of its own and back.
- Hints are a card above the line, as in IntelliJ: rounded, with a shadow, the instruction's syntax in the code
  font above the description.
- The Edit, View and Run menus are grouped into submenus; the menus open on hover and close when the mouse leaves.
- Fixed: closing a tab could crash the editor; dragging the splitters left stale pixels behind.

## 1.2.1

A new look, in the style of the IntelliJ New UI.

- The panels (project tree, editor, bottom panel, debugger, memory) are rounded islands on a darker window
  background; the gaps between them are the splitters.
- On Windows 11 the title bar takes the color of the toolbar.
- Editor tabs without boxes: the open tab is underlined (blue while it has the focus), the tab under the mouse gets
  a rounded highlight.
- A project widget on the toolbar: the project's initials and name; click it for the recent projects and
  "Open folder".
- Panel titles in normal case ("Project", "Outline", "Memory"), rounded inputs, toolbar buttons and drop-downs
  without frames.
- The README is in English (the Russian one is `README.ru.md`, also in the zip and the installer) and says openly
  that AsmEditor was written by an AI, Claude.
- MIT license.

## 1.2.0

The first public release: NASM editor with live error checking, analyzer and quick fixes; build and run with
NASM + MSVC link or gcc, and Linux through WSL; a debugger with conditional breakpoints, logpoints, data
breakpoints, step back, attach, a built-in disassembler and two profilers; 9 lessons and 13 exercises; installer,
portable zip and self-update from GitHub releases.
