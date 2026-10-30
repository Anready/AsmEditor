#pragma once

#include "common.h"

// Context-aware word completion for NASM source.
namespace Completion {

enum class Kind : int {  // also the Scintilla image id of the list icon
    Instruction = 1, Register, Directive, Preprocessor, Keyword, Label, Macro, Function, Section, Snippet
};

struct Symbol {
    std::string name;
    Kind kind;
    int line = 0;              // 1-based line where it appears (0 for built-in words)
    bool declaration = false;  // extern/global: names something defined elsewhere
    std::string scope;         // for local labels (.loop): the global label they belong to
};

// Labels, equ constants, %define/%macro names, extern/global names declared in a source text.
std::vector<Symbol> ScanSymbols(const std::string& text);

// Builds the Scintilla list ("word?kind word?kind ...") for the word being typed at the end of lineBeforeWord + prefix.
// Returns an empty string when nothing should be offered (comment, string, no matches).
std::string BuildList(const std::string& lineBeforeWord, const std::string& prefix,
                      const std::vector<Symbol>& symbols);

struct Occurrence {
    int line;    // 1-based
    int column;  // 0-based byte offset in the line
    int length;  // bytes matched
    bool definition;
    bool qualified;  // scope.local form (for a local name) or the name. prefix of name.local (for a global one)
    std::string lineText;
};

// Whole-word uses of `name` outside comments and strings. For a local label (.loop) pass its owner in `scope`:
// only uses inside that scope count, plus the qualified form scope.loop anywhere in the text.
// With withQualifiedPrefix, a global name also matches as the prefix of qualified local labels (main in main.loop).
std::vector<Occurrence> FindOccurrences(const std::string& text, const std::string& name, const std::string& scope,
                                        bool withQualifiedPrefix = false);

// A valid NASM identifier (labels, constants, macros).
bool IsValidName(const std::string& name);
// A word NASM reserves (register, instruction, directive, size keyword).
bool IsReservedWord(const std::string& name);

// Color of the list icon for a kind (matches the syntax colors).
COLORREF KindColor(Kind kind);

}
