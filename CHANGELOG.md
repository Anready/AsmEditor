# Changelog

The release workflow publishes the section of the tagged version as the release notes.

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
