#pragma once

#include <string>
#include <vector>

// Short Russian reference for x86-64 instructions, registers and NASM directives.
namespace Hints {

// Full card for a word: first line is the syntax (the caller highlights it), then description and flags.
// Empty when the word is unknown.
std::string For(const std::string& word);

// One-line syntax help shown while typing operands of an instruction.
std::string Signature(const std::string& mnemonic);

// Length in bytes of the first line of a card (to highlight it).
size_t HeaderLength(const std::string& card);

// C runtime / WinAPI function: prototype and in which registers (Win64 calling convention) the arguments go.
// Empty for unknown names.
std::string Function(const std::string& name);

// Parameter names of a known function's prototype ("format" for printf); variadic for "...". False: unknown.
bool Parameters(const std::string& name, std::vector<std::string>& names, bool& variadic, std::string& prototype);

// Every instruction mnemonic the reference knows (for tests and completeness checks).
std::vector<std::string> KnownInstructions();

}
