// AsmEditor.exe --ui-test <folder>: drives the real window through WM_COMMAND (open a project, debug, step,
// step back, the flags tab, the profiler), checks what the window shows, saves screenshots to the folder and
// compares them with tests\ui-baseline\*.png when a baseline of the same size exists. Exit code = failed checks.
#include "mw_internal.h"

#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")

namespace {

const char* kProgram =
    "default rel\r\n"                 // 1
    "extern printf\r\n"               // 2
    "global main\r\n"                 // 3
    "section .data\r\n"               // 4
    "    counter dq 0\r\n"            // 5
    "    fmt db \"%lld\", 10, 0\r\n"  // 6
    "section .text\r\n"               // 7
    "main:\r\n"                       // 8
    "    push rbx\r\n"                // 9
    "    sub rsp, 32\r\n"             // 10
    "    xor ebx, ebx\r\n"            // 11
    ".loop:\r\n"                      // 12
    "    inc qword [counter]\r\n"     // 13
    "    inc rbx\r\n"                 // 14
    "    cmp rbx, 5\r\n"              // 15
    "    jl .loop\r\n"                // 16
    "    lea rcx, [fmt]\r\n"          // 17
    "    mov rdx, [counter]\r\n"      // 18
    "    call printf\r\n"             // 19
    "    add rsp, 32\r\n"             // 20
    "    pop rbx\r\n"                 // 21
    "    xor eax, eax\r\n"            // 22
    "    ret\r\n";                    // 23

CLSID PngEncoder() {
    UINT count = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&count, &size);
    std::vector<uint8_t> buf(size);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    Gdiplus::GetImageEncoders(count, size, codecs);
    for (UINT i = 0; i < count; i++) {
        if (wcscmp(codecs[i].MimeType, L"image/png") == 0) return codecs[i].Clsid;
    }
    return {};
}

// Share of pixels that differ noticeably (0..1); -1 when the sizes differ.
double Difference(Gdiplus::Bitmap& a, Gdiplus::Bitmap& b) {
    if (a.GetWidth() != b.GetWidth() || a.GetHeight() != b.GetHeight()) return -1;
    UINT w = a.GetWidth(), h = a.GetHeight();
    Gdiplus::Rect r(0, 0, w, h);
    Gdiplus::BitmapData da{}, db{};
    a.LockBits(&r, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &da);
    b.LockBits(&r, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &db);
    uint64_t differ = 0;
    for (UINT y = 0; y < h; y++) {
        auto* pa = reinterpret_cast<const uint8_t*>(da.Scan0) + y * da.Stride;
        auto* pb = reinterpret_cast<const uint8_t*>(db.Scan0) + y * db.Stride;
        for (UINT x = 0; x < w * 4; x += 4) {
            int d = std::max({std::abs(pa[x] - pb[x]), std::abs(pa[x + 1] - pb[x + 1]), std::abs(pa[x + 2] - pb[x + 2])});
            if (d > 48) differ++;
        }
    }
    a.UnlockBits(&da);
    b.UnlockBits(&db);
    return static_cast<double>(differ) / (static_cast<double>(w) * h);
}
}

class UiTest {
public:
    UiTest(MainWindow& w, fs::path out) : w_(w), out_(std::move(out)) {
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&gdiplus_, &input, nullptr);
    }
    ~UiTest() { Gdiplus::GdiplusShutdown(gdiplus_); }

    // Called every 100 ms. Each step runs once its condition is met (or fails after 30 s).
    void Tick() {
        if (step_ >= static_cast<int>(steps_.size())) return;
        if (GetTickCount64() - stepStart_ > 30000) {
            Fail("timeout in step " + std::to_string(step_) + " (" + steps_[step_].name + ")");
            Next();
            return;
        }
        if (steps_[step_].run()) Next();
    }

private:
    struct Step {
        std::string name;
        std::function<bool()> run;  // true: done, go on
    };
    MainWindow& w_;
    fs::path out_;
    ULONG_PTR gdiplus_ = 0;
    int step_ = 0;
    ULONGLONG stepStart_ = GetTickCount64();
    int failed_ = 0;
    std::string report_;
    fs::path project_;
    int lastLine_ = 0;

    void Note(const std::string& text) {
        report_ += text + "\n";
        Log::Write("ui-test: " + text);
    }
    void Check(bool ok, const std::string& what) {
        Note(std::string(ok ? "ok    " : "FAIL  ") + what);
        if (!ok) failed_++;
    }
    void Fail(const std::string& what) { Check(false, what); }
    void Command(int id) { PostMessageW(w_.hwnd_, WM_COMMAND, id, 0); }
    int ExecLine() {
        if (!w_.DebugStopped()) return 0;
        return w_.debugger_->LocationOf(w_.debugger_->Context().Rip).line;
    }
    void Next() {
        step_++;
        stepStart_ = GetTickCount64();
        if (step_ == static_cast<int>(steps_.size())) Finish();
    }
    bool Waited(ULONGLONG ms) const { return GetTickCount64() - stepStart_ >= ms; }

    void Screenshot(const std::string& name) {
        RedrawWindow(w_.hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        RECT rc;
        GetWindowRect(w_.hwnd_, &rc);
        int width = rc.right - rc.left, height = rc.bottom - rc.top;
        HDC screen = GetDC(nullptr);
        HDC dc = CreateCompatibleDC(screen);
        HBITMAP bmp = CreateCompatibleBitmap(screen, width, height);
        HGDIOBJ old = SelectObject(dc, bmp);
        PrintWindow(w_.hwnd_, dc, PW_RENDERFULLCONTENT);
        SelectObject(dc, old);
        {
            Gdiplus::Bitmap image(bmp, nullptr);
            CLSID png = PngEncoder();
            fs::path file = out_ / Widen(name + ".png");
            Check(image.Save(file.c_str(), &png, nullptr) == Gdiplus::Ok, "screenshot " + name);
            fs::path baseline = fs::path(__FILE__).parent_path().parent_path() / L"tests" / L"ui-baseline" / Widen(name + ".png");
            if (FileExists(baseline.wstring())) {
                Gdiplus::Bitmap expected(baseline.c_str());
                double d = Difference(image, expected);
                char buf[96];
                if (d < 0) sprintf_s(buf, "baseline %s has another size, not compared", name.c_str());
                else sprintf_s(buf, "screenshot %s matches the baseline (%.2f%% of pixels differ)", name.c_str(), d * 100);
                if (d < 0) Note(buf);
                else Check(d < 0.02, buf);
            }
        }
        DeleteObject(bmp);
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
    }

    void Finish() {
        Note(failed_ ? std::to_string(failed_) + " check(s) failed" : "all checks passed");
        std::ofstream(out_ / L"report.txt", std::ios::binary) << report_;
        w_.uiTestExit_ = failed_;
        if (w_.debugger_) w_.debugger_.reset();
        PostMessageW(w_.hwnd_, WM_CLOSE, 0, 0);
    }

public:
    std::vector<Step> steps_ = {
        {"open project",
         [this] {
             project_ = out_ / L"project";
             std::error_code ec;
             fs::remove_all(project_, ec);
             fs::create_directories(project_, ec);
             std::ofstream(project_ / L"main.asm", std::ios::binary) << kProgram;
             SetWindowPos(w_.hwnd_, nullptr, 40, 40, 1280, 800, SWP_NOZORDER);
             w_.OpenPath(project_);
             w_.OpenFile(project_ / L"main.asm");
             return true;
         }},
        {"editor screenshot",
         [this] {
             if (!Waited(800)) return false;
             Editor* ed = w_.Active();
             Check(ed && ed->file().filename() == L"main.asm", "main.asm is open");
             Screenshot("01-editor");
             return true;
         }},
        {"breakpoint and debug",
         [this] {
             Editor* ed = w_.Active();
             if (!ed) return true;
             ed->GotoLine(13);
             Command(ID_TOGGLE_BP);
             Command(ID_DEBUG);
             return true;
         }},
        {"stopped at the breakpoint",
         [this] {
             if (!w_.DebugStopped()) return false;
             Check(ExecLine() == 13, "stopped on line 13 (got " + std::to_string(ExecLine()) + ")");
             Check(w_.debugger_->Context().Rbx == 0, "rbx = 0 on the first pass");
             w_.ShowDebugTab(MainWindow::DebugTab::Registers);
             Screenshot("02-debug");
             lastLine_ = ExecLine();
             Command(ID_TOGGLE_BP);  // off again
             Command(ID_STEP_OVER);
             return true;
         }},
        {"step over",
         [this] {
             if (!w_.DebugStopped() || ExecLine() == lastLine_) return false;
             Check(ExecLine() == 14, "a step goes to line 14");
             Command(ID_STEP_OVER);
             lastLine_ = ExecLine();
             return true;
         }},
        {"flags tab",
         [this] {
             if (!w_.DebugStopped() || ExecLine() == lastLine_) return false;
             Check(ExecLine() == 15, "the next step goes to line 15");
             w_.ShowDebugTab(MainWindow::DebugTab::Flags);
             std::string flags = w_.flagsView_->Text();
             Check(flags.find("SF") != std::string::npos && flags.find("jl") != std::string::npos, "the flags tab lists flags and conditions");
             Screenshot("03-flags");
             lastLine_ = ExecLine();
             Command(ID_STEP_BACK);
             return true;
         }},
        {"step back",
         [this] {
             if (!w_.DebugStopped() || ExecLine() == lastLine_) return false;
             Check(ExecLine() == 14, "step back returns to line 14");
             uint64_t counter = 0;
             auto addr = w_.debugger_->LookupSymbol("counter");
             if (addr) w_.debugger_->ReadMemory(*addr, &counter, 8);
             Check(counter == 1, "counter is 1 after stepping back");
             Command(ID_STOP);
             return true;
         }},
        {"debugging stopped",
         [this] {
             if (w_.debugging_) return false;
             Command(ID_PROFILE_RUN);
             return true;
         }},
        {"profiler",
         [this] {
             if (w_.profile_.empty()) return false;
             if (!Waited(500)) return false;
             uint64_t hits = 0;
             for (const ProfileLine& p : w_.profile_) {
                 if (p.location.line == 13) hits = p.hits;
             }
             Check(hits == 5, "the profiler counts 5 passes of line 13 (got " + std::to_string(hits) + ")");
             w_.ShowPane(MainWindow::Pane::Output);
             Screenshot("04-profile");
             return true;
         }},
    };
};

void MainWindow::StartUiTest(const fs::path& out) {
    std::error_code ec;
    fs::create_directories(out, ec);
    uiTest_ = std::make_unique<UiTest>(*this, out);
    SetTimer(hwnd_, kUiTestTimer, 100, nullptr);
}

void MainWindow::UiTestTick() {
    if (uiTest_) uiTest_->Tick();
}

MainWindow::MainWindow() = default;
MainWindow::~MainWindow() = default;
