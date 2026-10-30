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

namespace Analyzer {
// Code checks (warnings): a callee-saved register (rbx rbp rsi rdi r12–r15) changed and not saved; div/idiv without
// preparing rdx; instructions after ret/jmp that no label leads to.
std::vector<Diagnostic> Analyze(const std::string& text);

struct Edit {
    int line = 0;           // 0-based
    bool replace = false;   // replace the line, or insert before it
    std::string text;       // without the line break
};
struct Fix {
    std::string title;      // Russian; the UI translates it
    std::vector<Edit> edits;
};
// Fixes for a diagnostic with a code (from Analyze, StackCheck or NASM's "symbol not defined").
std::vector<Fix> QuickFixes(const std::string& text, const Diagnostic& d);
// Applies edits (any order) to the text; `eol` is "\r\n" or "\n".
std::string Apply(const std::string& text, const std::vector<Edit>& edits, const std::string& eol);

// The arguments of "call name" on a line (0-based): which of the argument registers (and stack slots) were set
// in the lines above it, back to the previous call, label or ret.
struct Argument {
    std::string where;   // "rcx", "rdx", "r8", "r9", "[rsp+32]"…
    std::string name;    // parameter name from the prototype ("format"), or empty
    int setOnLine = -1;  // 0-based line that sets it, -1: not set
};
struct CallArguments {
    std::string function;
    std::string prototype;
    bool known = false;     // the prototype is known (C runtime / WinAPI)
    bool variadic = false;
    std::vector<Argument> arguments;  // the prototype's fixed arguments, or the registers set (unknown function)
    std::vector<Argument> extra;      // registers set beyond the fixed ones (for variadic functions)
};
std::optional<CallArguments> ArgumentsOfCall(const std::string& text, int line);
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

namespace Conditions {
struct Condition {
    std::string name;     // "l" (as in jl, cmovl, setl)
    std::string aliases;  // "nge"
    std::string formula;  // "SF ≠ OF"
    std::string meaning;  // "меньше (со знаком)" (Russian; the UI translates it)
    std::string values;   // "SF=1 OF=0"
    bool holds = false;
};
// The 16 condition codes (o no b ae e ne be a s ns p np l ge le g), evaluated for these flags.
std::vector<Condition> All(uint32_t eflags);

struct Instruction {
    enum class Kind { Jump, Move, Set, Loop, Unconditional } kind = Kind::Jump;
    std::string mnemonic;  // "jl", "cmovge", "loop", "jrcxz"
    Condition condition;   // for loops and jrcxz: formula and values about rcx
};
// Decodes a conditional instruction (jcc, cmovcc, setcc, loop/loope/loopne, jrcxz/jecxz, jmp) at the start of
// `code` and evaluates its condition. Nothing for other instructions.
std::optional<Instruction> Decode(const uint8_t* code, size_t size, uint32_t eflags, uint64_t rcx, bool is32);
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
