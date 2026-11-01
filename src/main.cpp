#include "crash.h"
#include "mainwindow.h"
#include "update.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' " \
                        "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_TREEVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    Scintilla_RegisterClasses(inst);
    SetEnvironmentVariableW(L"VSLANG", L"1033");  // English messages from MSVC tools

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    // AsmEditor.exe --ui-test <folder>: the UI self-test with its own clean settings folder.
    fs::path uiTest;
    if (argc >= 3 && wcscmp(argv[1], L"--ui-test") == 0) {
        uiTest = fs::absolute(argv[2]);
        SetEnvironmentVariableW(L"ASMEDITOR_HOME", (uiTest / L"home").c_str());
    }
    Crash::Install();

    {
        wchar_t self[MAX_PATH * 2];
        GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
        Update::CleanupOld(self);  // the exe replaced by the last update
    }
    Settings settings;
    settings.Load();
    Theme::Apply(settings.theme);
    SetLanguage(settings.language);
    Log::Enable(settings.log);
    InitDarkMode();
    RegisterFlatButton(inst);
    Toolchain::ApplyDefaults(settings);

    MainWindow window;
    if (!window.Create(inst, &settings)) return 1;

    if (!uiTest.empty()) {
        window.StartUiTest(uiTest);
    } else {
        // AsmEditor.exe [project folder] [files…]
        // --crash-test: crashes on purpose once the window is up (checks the crash report).
        bool crashTest = argc >= 2 && wcscmp(argv[1], L"--crash-test") == 0;
        for (int i = crashTest ? 2 : 1; i < argc; i++) window.OpenPath(NormalizePath(argv[i]));
        if (argc <= 1) window.RestoreSession();
        window.OfferCrashReport();
        if (crashTest) {
            volatile int* nowhere = nullptr;
            *nowhere = 1;
        }
    }
    LocalFree(argv);

    MSG msg;
    int exitCode = 0;
    while (true) {
        BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0) {
            if (got == 0) exitCode = static_cast<int>(msg.wParam);  // WM_QUIT: failed UI test checks
            break;
        }
        if (window.PreTranslate(msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    window.RestartIfUpdated();
    Scintilla_ReleaseResources();
    CoUninitialize();
    return exitCode;
}
