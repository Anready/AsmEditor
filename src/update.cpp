#include "update.h"

#include <bcrypt.h>
#include <sstream>
#include <winhttp.h>

#include <fstream>

#pragma comment(lib, "bcrypt.lib")

namespace Update {
namespace {

// ASMEDITOR_UPDATE_FEED=<file.json>: the release from a local file instead of GitHub (for testing the update).
std::wstring Feed() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"ASMEDITOR_UPDATE_FEED", buf, static_cast<DWORD>(std::size(buf)));
    return n && n < std::size(buf) ? std::wstring(buf, n) : std::wstring();
}


// The JSON string starting at s[i] (a quote); i moves past it.
std::string JsonString(const std::string& s, size_t& i) {
    std::string out;
    if (i >= s.size() || s[i] != '"') return out;
    for (i++; i < s.size() && s[i] != '"'; i++) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            out += s[i];
            continue;
        }
        char e = s[++i];
        switch (e) {
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
            unsigned cp = static_cast<unsigned>(strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16));
            i += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF && s.compare(i + 1, 2, "\\u") == 0) {  // surrogate pair
                unsigned lo = static_cast<unsigned>(strtoul(s.substr(i + 3, 4).c_str(), nullptr, 16));
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i += 6;
            }
            std::wstring w;
            if (cp >= 0x10000) {
                w += static_cast<wchar_t>(0xD800 + ((cp - 0x10000) >> 10));
                w += static_cast<wchar_t>(0xDC00 + ((cp - 0x10000) & 0x3FF));
            } else {
                w += static_cast<wchar_t>(cp);
            }
            out += Narrow(w);
            break;
        }
        default: out += e;
        }
    }
    i++;
    return out;
}

// The string value of "key" at the top level of the object (the first occurrence after `from`).
std::string Field(const std::string& json, const std::string& key, size_t from = 0, size_t* at = nullptr) {
    std::string quoted = "\"" + key + "\"";
    size_t k = json.find(quoted, from);
    if (k == std::string::npos) return {};
    size_t colon = json.find(':', k + quoted.size());
    size_t v = colon == std::string::npos ? std::string::npos : json.find_first_not_of(" \t\r\n", colon + 1);
    if (v == std::string::npos || json[v] != '"') return {};
    if (at) *at = k;
    return JsonString(json, v);
}

HINTERNET Session() {
    HINTERNET s = WinHttpOpen(L"AsmEditor-updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (s) WinHttpSetTimeouts(s, 10000, 10000, 15000, 60000);
    return s;
}

// One GET; the body goes to `sink`. Returns the HTTP status (0: no connection).
int Request(const std::wstring& host, const std::wstring& path, const std::function<bool(const char*, DWORD)>& sink) {
    int status = 0;
    HINTERNET session = Session();
    HINTERNET connect = session ? WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                : nullptr;
    const wchar_t* headers = L"Accept: application/vnd.github+json\r\nUser-Agent: AsmEditor\r\n";
    if (request && WinHttpSendRequest(request, headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD code = 0, size = sizeof(code);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &code, &size, WINHTTP_NO_HEADER_INDEX);
        status = static_cast<int>(code);
        std::vector<char> buf(64 * 1024);
        for (DWORD got = 0; WinHttpReadData(request, buf.data(), static_cast<DWORD>(buf.size()), &got) && got;) {
            if (!sink(buf.data(), got)) {
                status = -1;
                break;
            }
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    return status;
}
}

std::optional<Release> ParseRelease(const std::string& json) {
    Release r;
    r.tag = Field(json, "tag_name");
    if (r.tag.empty()) return std::nullopt;
    r.version = r.tag[0] == 'v' || r.tag[0] == 'V' ? r.tag.substr(1) : r.tag;
    r.page = Field(json, "html_url");
    r.notes = Field(json, "body");
    // Assets: each object has "digest" (when GitHub computed it) somewhere before its "browser_download_url".
    size_t assets = json.find("\"assets\"");
    size_t prev = assets == std::string::npos ? json.size() : assets;
    for (size_t at = prev;;) {
        size_t found = 0;
        std::string url = Field(json, "browser_download_url", at, &found);
        if (url.empty()) break;
        std::string digest;
        for (size_t d = json.find("\"digest\"", prev); d != std::string::npos && d < found; d = json.find("\"digest\"", d + 1)) {
            digest = Field(json, "digest", d);
        }
        if (digest.rfind("sha256:", 0) == 0) digest = digest.substr(7);
        else digest.clear();
        std::string name = url.substr(url.rfind('/') + 1);
        if (name == "AsmEditor.exe") r.exeUrl = url, r.exeSha256 = digest;
        else if (name.rfind("AsmEditor-setup", 0) == 0 && name.size() > 4 && name.substr(name.size() - 4) == ".exe") {
            r.setupUrl = url, r.setupSha256 = digest;
        }
        prev = at = found + 1;
    }
    return r;
}

int CompareVersions(const std::string& a, const std::string& b) {
    std::istringstream x(a), y(b);
    for (int i = 0; i < 4; i++) {
        std::string pa, pb;
        std::getline(x, pa, '.');
        std::getline(y, pb, '.');
        long va = atol(pa.c_str()), vb = atol(pb.c_str());
        if (va != vb) return va < vb ? -1 : 1;
    }
    return 0;
}

std::optional<std::string> Get(const std::wstring& host, const std::wstring& path, int& status) {
    std::string body;
    status = Request(host, path, [&](const char* p, DWORD n) {
        body.append(p, n);
        return body.size() < (16u << 20);
    });
    if (status <= 0) return std::nullopt;
    return body;
}

bool Download(const std::wstring& url, const fs::path& to, std::string& error) {
    const std::wstring prefix = L"https://";
    if (url.rfind(L"file:///", 0) == 0 && !Feed().empty()) {  // only with a local test feed
        std::error_code ec;
        fs::copy_file(fs::path(url.substr(8)), to, fs::copy_options::overwrite_existing, ec);
        if (ec) error = ec.message();
        return !ec;
    }
    if (url.rfind(prefix, 0) != 0) {
        error = Tr(std::string("адрес загрузки не https"));
        return false;
    }
    size_t slash = url.find(L'/', prefix.size());
    std::wstring host = url.substr(prefix.size(), slash - prefix.size()), path = slash == std::wstring::npos ? L"/" : url.substr(slash);
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = Tr(std::string("не удалось создать файл ")) + Narrow(to.wstring());
        return false;
    }
    int status = Request(host, path, [&](const char* p, DWORD n) {
        out.write(p, n);
        return static_cast<bool>(out);
    });
    out.close();
    if (status != 200) {
        error = Tr(std::string("загрузка не удалась, HTTP ")) + std::to_string(status);
        std::error_code ec;
        fs::remove(to, ec);
        return false;
    }
    return true;
}

std::string Sha256(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    std::string out;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<char> buf(1 << 16);
        while (in.read(buf.data(), buf.size()) || in.gcount() > 0) {
            BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()), static_cast<ULONG>(in.gcount()), 0);
        }
        UCHAR digest[32];
        if (BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
            char hex[3];
            for (UCHAR b : digest) {
                sprintf_s(hex, "%02x", b);
                out += hex;
            }
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

std::optional<Release> Latest(std::string& error) {
    if (std::wstring feed = Feed(); !feed.empty()) {
        std::ifstream in(fs::path(feed), std::ios::binary);
        std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto r = ParseRelease(json);
        if (!r) error = Tr(std::string("непонятный ответ GitHub"));
        return r;
    }
    int status = 0;
    auto body = Get(kHost, kLatestPath, status);
    if (!body) {
        error = Tr(std::string("нет связи с GitHub"));
        return std::nullopt;
    }
    if (status == 404) {  // no release published yet
        error.clear();
        return std::nullopt;
    }
    if (status != 200) {
        error = "GitHub: HTTP " + std::to_string(status);
        return std::nullopt;
    }
    auto r = ParseRelease(*body);
    if (!r) error = Tr(std::string("непонятный ответ GitHub"));
    return r;
}

bool IsInstalled(const fs::path& exe) {
    std::error_code ec;
    return fs::exists(exe.parent_path() / L"unins000.exe", ec);
}

bool Verify(const fs::path& file, const std::string& sha256, std::string& error) {
    char mz[2] = {};
    std::ifstream in(file, std::ios::binary);
    in.read(mz, 2);
    std::error_code ec;
    if (!in || mz[0] != 'M' || mz[1] != 'Z' || fs::file_size(file, ec) < 100 * 1024) {
        error = Tr(std::string("скачанный файл — не программа Windows"));
        return false;
    }
    if (!sha256.empty() && Sha256(file) != sha256) {
        error = Tr(std::string("контрольная сумма SHA-256 не совпала"));
        return false;
    }
    return true;
}

bool ReplaceExecutable(const fs::path& exe, const fs::path& downloaded, std::string& error) {
    fs::path old = exe;
    old += L".old";
    MoveFileExW(old.c_str(), nullptr, 0);  // a leftover from an earlier update
    DeleteFileW(old.c_str());
    if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        error = Tr(std::string("не удалось переименовать ")) + Narrow(exe.wstring()) + " (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    if (!MoveFileExW(downloaded.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        DWORD code = GetLastError();
        MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);  // put the old one back
        error = Tr(std::string("не удалось записать новую версию")) + " (" + std::to_string(code) + ")";
        return false;
    }
    return true;
}

void CleanupOld(const fs::path& exe) {
    fs::path old = exe;
    old += L".old";
    DeleteFileW(old.c_str());
}

}
