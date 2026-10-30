#include "mainwindow.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' " \
                        "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_TREEVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    Scintilla_RegisterClasses(inst);
    SetEnvironmentVariableW(L"VSLANG", L"1033");  // English messages from MSVC tools

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

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    // AsmEditor.exe [project folder] [files…]
    for (int i = 1; i < argc; i++) window.OpenPath(NormalizePath(argv[i]));
    if (argc <= 1) window.RestoreSession();
    LocalFree(argv);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (window.PreTranslate(msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Scintilla_ReleaseResources();
    CoUninitialize();
    return 0;
}
