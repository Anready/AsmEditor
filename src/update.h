#pragma once

#include "common.h"

// Updates of AsmEditor itself from the GitHub releases of Anready/AsmEditor. A portable copy replaces its own exe
// (AsmEditor.exe of the release, checked against the SHA-256 GitHub publishes); an installed copy runs the new
// installer. UI-independent (covered by the tests); MainWindow drives it.
namespace Update {

constexpr const wchar_t* kHost = L"api.github.com";
constexpr const wchar_t* kLatestPath = L"/repos/Anready/AsmEditor/releases/latest";

struct Release {
    std::string tag;          // "v1.2.1"
    std::string version;      // "1.2.1"
    std::string notes;        // the release text
    std::string page;         // its web page
    std::string exeUrl, exeSha256;
    std::string setupUrl, setupSha256;
};

std::optional<Release> ParseRelease(const std::string& json);
int CompareVersions(const std::string& a, const std::string& b);  // <0, 0, >0 ("1.10.0" > "1.9.3")

// GET over HTTPS (follows redirects). Nothing on a network error; `status` gets the HTTP status.
std::optional<std::string> Get(const std::wstring& host, const std::wstring& path, int& status);
bool Download(const std::wstring& url, const fs::path& to, std::string& error);  // a full https:// URL
std::string Sha256(const fs::path& file);  // lower-case hex, "" on error

// The latest release; nothing with a reason in `error` ("" when there simply is no release yet).
std::optional<Release> Latest(std::string& error);

// An installed copy has the Inno Setup uninstaller next to it.
bool IsInstalled(const fs::path& exe);
// The running exe -> exe.old, the downloaded file -> exe (a running exe may be renamed, not overwritten).
bool ReplaceExecutable(const fs::path& exe, const fs::path& downloaded, std::string& error);
void CleanupOld(const fs::path& exe);  // removes exe.old left by the previous update
// The downloaded file is a Windows program (MZ + PE) and, when GitHub gave a digest, has that SHA-256.
bool Verify(const fs::path& file, const std::string& sha256, std::string& error);

}
