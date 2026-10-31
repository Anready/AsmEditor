// Updates of AsmEditor itself (Help → Update AsmEditor, and a check once a day at start): see update.h.
#include "mw_internal.h"
#include "update.h"
#include "version.h"

namespace {
struct UpdateInfo {
    std::optional<Update::Release> release;
    std::string error;
    bool interactive = false;
};
struct UpdateDone {
    bool ok = false;
    bool installer = false;
    fs::path file;  // the downloaded exe or installer
    std::string error;
    std::string version;
};

fs::path ThisExe() {
    wchar_t exe[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    return exe;
}
}

// The check at start: at most once a day, not in the UI test, and not when switched off in the settings.
void MainWindow::MaybeCheckForUpdate() {
    if (uiTest_ || !settings_->checkUpdates) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    int today = t.wYear * 10000 + t.wMonth * 100 + t.wDay;
    if (settings_->lastUpdateCheck == today) return;
    settings_->lastUpdateCheck = today;
    settings_->Save();
    CheckForAppUpdate(false);
}

void MainWindow::CheckForAppUpdate(bool interactive) {
    if (interactive) Print(Tr(std::string("Проверка обновлений AsmEditor…")) + "\n", Out::Info);
    HWND hwnd = hwnd_;
    std::thread([hwnd, interactive] {
        auto* info = new UpdateInfo;
        info->release = Update::Latest(info->error);
        info->interactive = interactive;
        PostMessageW(hwnd, WM_APP_UPDATE_INFO, 0, reinterpret_cast<LPARAM>(info));
    }).detach();
}

void MainWindow::OnUpdateInfo(LPARAM lp) {
    std::unique_ptr<UpdateInfo> info(reinterpret_cast<UpdateInfo*>(lp));
    const std::string current = ASMEDITOR_VERSION_TEXT;
    if (!info->release) {
        if (info->interactive) {
            Print((info->error.empty() ? Tr(std::string("Релизов AsmEditor на GitHub пока нет.")) : Tr(std::string("Не удалось проверить обновления: ")) + info->error) +
                      "\n", info->error.empty() ? Out::Info : Out::Warning);
        }
        return;
    }
    const Update::Release& r = *info->release;
    if (Update::CompareVersions(r.version, current) <= 0) {
        if (info->interactive) Print(Tr(std::string("У вас последняя версия AsmEditor: ")) + current + "\n", Out::Success);
        return;
    }
    if (!info->interactive) {  // at start: a note, not a dialog
        Print(Tr(std::string("Вышла новая версия AsmEditor: ")) + r.version + " (" + Tr(std::string("у вас")) + " " + current + "). " +
                  Tr(std::string("Справка → «Обновить AsmEditor…».")) + "\n", Out::Info);
        pendingRelease_ = r;
        return;
    }
    std::string notes = r.notes.size() > 700 ? r.notes.substr(0, 700) + "…" : r.notes;
    std::wstring q = std::wstring(Tr(L"Вышла новая версия AsmEditor: ")) + Widen(r.version) + L" (" + Tr(L"у вас") + L" " +
                     Widen(current) + L").\n\n" + Widen(notes) + L"\n\n" + Tr(L"Скачать и установить её сейчас?");
    if (MessageBoxW(hwnd_, q.c_str(), L"AsmEditor", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    InstallUpdate(r);
}

// Downloads the new version: the installer for an installed copy, the exe for a portable one.
void MainWindow::InstallUpdate(const Update::Release& r) {
    fs::path exe = ThisExe();
    bool installer = Update::IsInstalled(exe);
    std::string url = installer ? r.setupUrl : r.exeUrl, sha = installer ? r.setupSha256 : r.exeSha256;
    if (url.empty()) {  // the release has no such file: let the user download by hand
        ShellExecuteW(hwnd_, L"open", Widen(r.page).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    wchar_t tmp[MAX_PATH + 1];
    GetTempPathW(MAX_PATH + 1, tmp);
    fs::path file = installer ? fs::path(tmp) / Widen("AsmEditor-setup-" + r.version + ".exe")
                              : fs::path(exe.wstring() + L".download");  // next to the exe: the rename stays on one disk
    ShowPane(Pane::Output);
    Print(Tr(std::string("Загрузка AsmEditor ")) + r.version + "…\n", Out::Info);
    HWND hwnd = hwnd_;
    std::string version = r.version;
    std::thread([hwnd, url, sha, file, installer, version] {
        auto* done = new UpdateDone;
        done->installer = installer;
        done->file = file;
        done->version = version;
        done->ok = Update::Download(Widen(url), file, done->error) && Update::Verify(file, sha, done->error);
        if (!done->ok) {
            std::error_code ec;
            fs::remove(file, ec);
        }
        PostMessageW(hwnd, WM_APP_UPDATE_DONE, 0, reinterpret_cast<LPARAM>(done));
    }).detach();
}

void MainWindow::OnUpdateDone(LPARAM lp) {
    std::unique_ptr<UpdateDone> done(reinterpret_cast<UpdateDone*>(lp));
    if (!done->ok) {
        Print(Tr(std::string("Обновление не удалось: ")) + done->error + "\n", Out::Error);
        return;
    }
    if (done->installer) {  // Inno Setup closes and restarts AsmEditor itself
        Print(Tr(std::string("Запуск установщика новой версии…")) + "\n", Out::Info);
        ShellExecuteW(hwnd_, L"open", done->file.c_str(), L"/SILENT /SP- /CLOSEAPPLICATIONS /RESTARTAPPLICATIONS", nullptr,
                      SW_SHOWNORMAL);
        return;
    }
    std::string error;
    if (!Update::ReplaceExecutable(ThisExe(), done->file, error)) {
        Print(Tr(std::string("Обновление не удалось: ")) + error + "\n", Out::Error);
        return;
    }
    Print(Tr(std::string("AsmEditor обновлён до версии ")) + done->version + ". " +
              Tr(std::string("Новая версия запустится после закрытия окна.")) + "\n", Out::Success);
    restartAfterClose_ = true;
    PostMessageW(hwnd_, WM_CLOSE, 0, 0);  // asks about unsaved files as usual
}

// After the window is gone: start the updated exe (it restores the session).
void MainWindow::RestartIfUpdated() {
    if (!restartAfterClose_) return;
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + ThisExe().wstring() + L"\"";
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}
