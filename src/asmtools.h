#pragma once

#include "common.h"

// Source-level tools that work on plain text (no UI): formatter, calling-convention check, disassembler output,
// snippets, project templates and exercises. Covered by the unit tests.

namespace AsmFormat {
struct Options {
    int indent = 4;     // column of mnemonics and data directives
    int operands = 12;  // column of operands
    int comments = 40;  // column of trailing comments
};
// Aligns mnemonics, operands and trailing comments in columns; labels stay at the left edge. Lines inside strings
// and %macro bodies keep their content, only the columns change. `eol` is "\r\n" or "\n".
std::string Format(const std::string& text, const Options& options, const std::string& eol);
std::string FormatLine(const std::string& line, const Options& options);
}

namespace StackCheck {
// Warnings (line, message) for "call" made with a stack that is probably not 16-byte aligned or without the
// 32 bytes of shadow space required by the Win64 calling convention. Tracks rsp changes (push/pop/sub/add/and)
// from the start of each global label; gives up on a function when rsp changes in a way it cannot follow.
std::vector<Diagnostic> Analyze(const std::string& text);
}

namespace Disasm {
struct Line {
    uint64_t address = 0;
    std::string bytes;  // hex
    std::string text;   // instruction
};
std::vector<Line> Parse(const std::string& ndisasmOutput);
// Runs ndisasm on the given bytes (written to a temporary file).
std::vector<Line> Run(const fs::path& ndisasm, const uint8_t* data, size_t size, uint64_t address, int bits);
}

namespace Snippets {
struct Snippet {
    const char* name;   // what is typed / shown in the list
    const char* title;  // description (Russian)
    const char* body;   // "|" marks the caret; lines are written at their final columns
};
const std::vector<Snippet>& All();
// Text to insert at the start of a line; caret = offset of the caret in it.
std::string Expand(const Snippet& s, const std::string& eol, size_t& caret);
}

namespace Templates {
struct Template {
    const wchar_t* title;
    const wchar_t* fileName;
    const char* text;
};
const std::vector<Template>& All();
}

namespace Exercises {
struct Test {
    std::string input;     // typed into the program
    std::string expected;  // its output must contain this (whitespace-insensitive)
};
struct Exercise {
    std::string id;
    std::wstring title;
    std::string description;  // task text (goes into the starter file as a comment)
    std::string starter;      // code to begin with
    std::vector<Test> tests;
};
const std::vector<Exercise>& All();
const Exercise* Find(const std::string& id);
// "; task: sum" in the first lines of a solution names its exercise.
std::string IdFromSource(const std::string& text);
// Output matches when both are equal after collapsing whitespace.
bool OutputMatches(const std::string& output, const std::string& expected);
}
