#pragma once

#include "common.h"

// When the editor crashes it writes a minidump to the settings folder (crashes\); the next start offers to report
// the crash on GitHub with what is known about it.
namespace Crash {
void Install();

struct Report {
    fs::path dump;
    std::wstring code;   // "0xC0000005"
    std::wstring where;  // "AsmEditor.exe+0x1A2B3"
};
// The crash of an earlier run that was not offered yet (it is offered once).
std::optional<Report> TakePending();
// The "new issue" page of the project with the report filled in.
std::wstring IssueUrl(const Report& report);
}
