# Changelog

The release workflow publishes the section of the tagged version as the release notes.

## 1.5.0

- Inlay hints, as in IntelliJ: the parameter a register gets before a `call` (`format → printf`), the size of a
  memory operand implied by the register, and the values of registers next to the lines while debugging. View →
  Inlay hints turns them off.
- Search Everywhere on Shift pressed twice: files, the labels and macros of the project, and commands in one list.
- Recent files (Ctrl+E) and a switcher on Ctrl+Tab: the open files in the order you used them; release Ctrl to go.
- Block guides: a thin line down each label body and `%macro`/`%if`/`%rep` block, brighter for the one the caret
  is in (the fold margin highlights it too).
- The gutter marks lines added, changed and deleted since the last git commit (or since the file was saved when it
  is not in git); the scrollbar shows them too.
- Breadcrumbs in the status bar: project › folders › file › section › label › local label; click one to go there.
- Pixel-smooth scrolling with the mouse wheel and the touchpad, up and down and sideways; it starts at once, stops
  on a whole line without a jerk, and the scrollbar thumbs, the sticky lines and the change marks move with it
  without flicker (also in a split editor).
- Lines with problems are no longer filled with red or yellow (the squiggles and the scrollbar marks show them);
  breakpoint lines get a red background instead.
- Scrolling over many problems is as fast as over clean code: the squiggles were drawn as small images, each frame
  took about 13 ms instead of 3; hint cards no longer pop up while the text scrolls under the pointer.
- After a crash, AsmEditor writes a minidump and on the next start offers to report the crash on GitHub.
- Packaging: a Scoop manifest, winget manifests, a project page for GitHub Pages and optional SignPath signing in
  the release workflow (see packaging/README.md).

## 1.4.0

- Every scrollbar is IntelliJ-style: a thin rounded thumb over the edge of the editor, the panels, the project tree,
  the outline and the lists of the dialogs, vertical and horizontal, all as thick as each other.
- Smooth scrolling: a notch of the mouse wheel scrolls in a few steps instead of jumping.
- Sticky lines, as in IntelliJ: the section, the global label and the %macro/%if/%rep blocks you are in stay at the
  top of the editor while it scrolls, with their line numbers; click one to go to it.
- File icons by type (assembly, include, C/C++, Python, text, configs, scripts, binaries, images…) in the project
  tree, the tabs and Go to File (Ctrl+P).

## 1.3.1

- Scrolling stops 5 empty lines past the end of the code instead of a whole page, in the editor and every panel.
- One scrollbar in the editor, as in IntelliJ: a rounded thumb over the marks of the whole file (problems,
  breakpoints, the execution line, search hits). Drag the thumb, click a mark to go to its line, click elsewhere to
  jump there.

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
