#pragma once

#include "common.h"

#include <set>

// Which files of a project folder make up one program.
namespace Project {

std::wstring Key(const fs::path& p);  // normalized lower-case path, for comparisons
bool DefinesMain(const fs::path& file);  // has a main / wmain / WinMain label
bool IsSourceFile(const fs::path& p);  // assembled on its own (.asm/.s/.nasm); .inc/.mac are only included

// Files pulled in by %include (transitively) that exist on disk.
std::vector<fs::path> Dependencies(const fs::path& file, const std::vector<fs::path>& includeDirs);

struct Sources {
    std::vector<fs::path> files;          // to assemble and link; the target comes first
    std::vector<fs::path> otherPrograms;  // skipped: they define their own main
};

// All sources of the project for building `target`: every source file in the folder, except files that are
// %include'd by others, files excluded by the user, and other programs (files with their own main).
Sources Collect(const fs::path& root, const fs::path& target, const std::set<std::wstring>& excluded);

}
