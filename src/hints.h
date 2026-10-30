#pragma once

#include <string>

// Short Russian reference for x86-64 instructions, registers and NASM directives.
namespace Hints {

// Full card for a word: first line is the syntax (the caller highlights it), then description and flags.
// Empty when the word is unknown.
std::string For(const std::string& word);

// One-line syntax help shown while typing operands of an instruction.
std::string Signature(const std::string& mnemonic);

// Length in bytes of the first line of a card (to highlight it).
size_t HeaderLength(const std::string& card);

}
