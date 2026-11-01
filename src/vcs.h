#pragma once

#include "common.h"

namespace Vcs {
// The file as of the last commit (git show HEAD:<file>, UTF-8), or nothing: no git, not in a repository, not
// committed. Runs git: call it off the UI thread.
std::optional<std::string> HeadText(const fs::path& file);
}
