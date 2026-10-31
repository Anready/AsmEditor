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

    void Screenshot(const std::string& name, HWND window = nullptr) {
        HWND target = window ? window : w_.hwnd_;
        RedrawWindow(target, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        RECT rc;
        GetWindowRect(target, &rc);
        int width = rc.right - rc.left, height = rc.bottom - rc.top;
        HDC screen = GetDC(nullptr);
        HDC dc = CreateCompatibleDC(screen);
        HBITMAP bmp = CreateCompatibleBitmap(screen, width, height);
        HGDIOBJ old = SelectObject(dc, bmp);
        PrintWindow(target, dc, PW_RENDERFULLCONTENT);
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
             w_.ShowDebugTab(MainWindow::DebugTab::Disasm);
             std::string code = w_.disasmView_->Text();
             Check(code.find("cmp rbx, 0x5") != std::string::npos && code.find("inc qword [rel 0x") != std::string::npos,
                   "the built-in disassembler shows the code of main");
             Screenshot("05-code");
             w_.ShowDebugTab(MainWindow::DebugTab::Flags);
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
        {"split into two tab groups",
         [this] {
             for (const wchar_t* name : {L"a.asm", L"b.asm", L"c.asm"}) {
                 std::ofstream(project_ / name, std::ios::binary) << "; " << Narrow(name) << "\r\n";
                 w_.OpenFile(project_ / name);
             }
             Editor* a = w_.editors_[w_.FindByPath(project_ / L"a.asm")].get();
             w_.SplitWith(w_.IndexOf(a), false);
             w_.OpenFile(project_ / L"b.asm");  // already open: stays in the first group
             Check(a->group == 1 && w_.SplitEditor() == a, "a.asm moved to the right half");
             // A file opened while the right half has the focus opens there.
             w_.Activate(w_.IndexOf(a));
             std::ofstream(project_ / L"d.asm", std::ios::binary) << "; d\r\n";
             Editor* d = w_.OpenFile(project_ / L"d.asm");
             Check(d && d->group == 1 && w_.GroupTabs(1).size() == 2, "the right half has its own tabs (a.asm, d.asm)");
             Check(w_.GroupTabs(0).size() == 3, "the left half keeps main.asm, b.asm, c.asm");
             return true;
         }},
        {"split screenshot",
         [this] {
             if (!Waited(300)) return false;
             Screenshot("06-split");
             Editor* c = w_.editors_[w_.FindByPath(project_ / L"c.asm")].get();
             w_.DetachEditor(w_.IndexOf(c), nullptr);
             Editor* b = w_.editors_[w_.FindByPath(project_ / L"b.asm")].get();
             Check(w_.floatGroups_.size() == 1 && w_.IsFloating(c), "c.asm has a window of its own");
             w_.MoveToGroup(w_.IndexOf(b), c->group);
             Check(w_.GroupTabs(c->group).size() == 2, "a second tab joins that window");
             return true;
         }},
        {"separate window screenshot",
         [this] {
             if (!Waited(300)) return false;
             if (w_.floatGroups_.empty()) {
                 Fail("no window of its own");
                 return true;
             }
             HWND f = w_.floatGroups_.begin()->second.hwnd;
             Screenshot("07-window", f);
             // Dropped on the main window's editor (as when dragged there by its title), its tabs come back.
             RECT area = w_.GroupArea(0);
             POINT at{(area.left + area.right) / 2, (area.top + area.bottom) / 2};
             ClientToScreen(w_.hwnd_, &at);
             PostMessageW(f, WM_APP + 1 /* WM_FLOAT_DOCK */, at.x, at.y);
             return true;
         }},
        {"window docked back",
         [this] {
             if (!w_.floatGroups_.empty() && !Waited(3000)) return false;
             Check(w_.floatGroups_.empty(), "the window closed after docking");
             Check(w_.GroupTabs(0).size() == 3 && w_.GroupTabs(1).size() == 2, "its tabs joined the left half");
             // A window of its own split in two, like the main editor.
             Editor* c = w_.editors_[w_.FindByPath(project_ / L"c.asm")].get();
             Editor* b = w_.editors_[w_.FindByPath(project_ / L"b.asm")].get();
             w_.DetachEditor(w_.IndexOf(c), nullptr);
             w_.MoveToGroup(w_.IndexOf(b), c->group);
             w_.SplitWith(w_.IndexOf(b), false);
             int second = w_.SecondHalf(c->group);
             Check(second && b->group == second && w_.WindowPrimary(second) == c->group, "the separate window is split");
             return true;
         }},
        {"split window screenshot",
         [this] {
             if (!Waited(300)) return false;
             if (w_.floatGroups_.empty()) {
                 Fail("no window of its own");
                 return true;
             }
             HWND f = w_.floatGroups_.begin()->second.hwnd;
             Screenshot("08-window-split", f);
             SendMessageW(f, WM_CLOSE, 0, 0);
             Check(w_.floatGroups_.empty() && w_.GroupTabs(0).size() == 3, "closing it brings both halves back");
             // Any window takes files and parts of the debugger alike: the registers get a window of their own,
             // the call stack and a file join them, the memory view goes into its second half.
             using P = MainWindow::TabItem;
             const int regs = static_cast<int>(MainWindow::DebugTab::Registers), stack = static_cast<int>(MainWindow::DebugTab::Stack);
             w_.DetachItem(P{-1, regs}, nullptr);
             int window = w_.partGroup_[regs];
             w_.MoveItem(P{-1, stack}, window);
             w_.MoveItem(P{w_.FindByPath(project_ / L"b.asm"), -1}, window);
             w_.SplitItem(P{-1, MainWindow::kMemoryPart}, window, true);
             int memoryGroup = w_.partGroup_[MainWindow::kMemoryPart];
             Check(window >= MainWindow::kFirstFloatGroup && w_.partGroup_[stack] == window &&
                       w_.GroupTabs(window).size() == 1, "a window of its own holds two parts and a file");
             Check(memoryGroup == w_.SecondHalf(window) && IsChild(w_.GroupHost(window), w_.memExpr_),
                   "the memory view is in its second half");
             Check(w_.PanelShown() && w_.GroupParts(MainWindow::kPanelGroup).size() == 3,
                   "the debug panel keeps the other three tabs");
             return true;
         }},
        {"mixed window screenshot",
         [this] {
             if (!Waited(300)) return false;
             HWND f = w_.GroupHost(w_.partGroup_[static_cast<int>(MainWindow::DebugTab::Registers)]);
             Screenshot("09-mixed-window", f);
             Screenshot("10-panel-without-parts");
             SendMessageW(f, WM_CLOSE, 0, 0);
             bool parts = true;
             for (int g : w_.partGroup_) parts &= g == MainWindow::kPanelGroup;
             Check(w_.floatGroups_.empty() && parts && IsChild(w_.hwnd_, w_.memExpr_),
                   "closing it: the file to the editor, the parts to the debug panel");
             w_.ResetDebugParts();
             Check(w_.partGroup_[MainWindow::kMemoryPart] == MainWindow::kPanelGroup2, "the panel is as at first again");
             // The whole debug panel in a window of its own, and back.
             w_.OnCommand(ID_DETACH_DEBUG);
             Check(!w_.PanelShown() && w_.floatGroups_.size() == 2, "the debug panel has a window of its own (split)");
             return true;
         }},
        {"debug window",
         [this] {
             if (!Waited(300)) return false;
             Screenshot("11-debug-window", w_.GroupHost(w_.partGroup_[0]));
             w_.OnCommand(ID_DETACH_DEBUG);
             Check(w_.floatGroups_.empty() && w_.PanelShown(), "and back into the main window");
             // With the mouse: a tab dragged onto the right third of the editor, then closed with its ✕.
             w_.Unsplit(0);
             for (const wchar_t* name : {L"x.asm", L"y.asm"}) {
                 std::ofstream(project_ / name, std::ios::binary) << "; " << Narrow(name) << "\r\n";
                 w_.OpenFile(project_ / name);
             }
             RedrawWindow(w_.hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
             int y = w_.FindByPath(project_ / L"x.asm");
             RECT tab = w_.tabRects_[y], ed = w_.rcEditor_;
             POINT from{(tab.left + tab.right) / 2, (tab.top + tab.bottom) / 2};
             POINT to{ed.right - (ed.right - ed.left) / 6, (ed.top + ed.bottom) / 2};
             SendMessageW(w_.hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(from.x, from.y));
             SendMessageW(w_.hwnd_, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(from.x + 40, from.y + 40));
             SendMessageW(w_.hwnd_, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(to.x, to.y));
             SendMessageW(w_.hwnd_, WM_LBUTTONUP, 0, MAKELPARAM(to.x, to.y));
             Check(w_.editors_[w_.FindByPath(project_ / L"x.asm")]->group == 1, "a tab dragged onto the right third splits");
             return true;
         }},
        {"close the moved tab",
         [this] {
             if (!Waited(300)) return false;
             RedrawWindow(w_.hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
             int y = w_.FindByPath(project_ / L"x.asm");
             if (y < 0) return true;
             RECT close = w_.closeRects_[y];
             POINT at{(close.left + close.right) / 2, (close.top + close.bottom) / 2};
             SendMessageW(w_.hwnd_, WM_MOUSEMOVE, 0, MAKELPARAM(at.x, at.y));
             SendMessageW(w_.hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(at.x, at.y));
             SendMessageW(w_.hwnd_, WM_LBUTTONUP, 0, MAKELPARAM(at.x, at.y));
             Check(w_.FindByPath(project_ / L"x.asm") < 0 && !w_.GroupHasItems(1), "its ✕ closes it and the split goes away");
             // The hint card of an instruction: above its line.
             Editor* ed = w_.OpenFile(project_ / L"main.asm");
             if (ed) {
                 Sci_Position line = 9;  // "    sub rsp, 32"
                 Sci_Position pos = ed->Call(SCI_POSITIONFROMLINE, line) + 4;
                 ed->Call(SCI_SETFIRSTVISIBLELINE, 0);
                 w_.ShowHint(ed, pos);
             }
             return true;
         }},
        {"hint card",
         [this] {
             if (!Waited(400)) return false;
             Editor* ed = w_.Active();
             RECT card{};
             if (Tip::Handle()) GetWindowRect(Tip::Handle(), &card);
             POINT line{0, ed ? static_cast<LONG>(ed->Call(SCI_POINTYFROMPOSITION, 0, ed->Call(SCI_POSITIONFROMLINE, 9))) : 0};
             if (ed) ClientToScreen(ed->hwnd(), &line);
             Check(Tip::Visible() && card.bottom <= line.y, "the hint card shows above the line");
             // From the screen: the card is a window of its own over the editor.
             RECT rc;
             GetWindowRect(w_.hwnd_, &rc);
             UnionRect(&rc, &rc, &card);
             int width = rc.right - rc.left, height = rc.bottom - rc.top;
             HDC screen = GetDC(nullptr);
             HDC dc = CreateCompatibleDC(screen);
             HBITMAP bmp = CreateCompatibleBitmap(screen, width, height);
             HGDIOBJ old = SelectObject(dc, bmp);
             BitBlt(dc, 0, 0, width, height, screen, rc.left, rc.top, SRCCOPY);
             SelectObject(dc, old);
             {
                 Gdiplus::Bitmap image(bmp, nullptr);
                 CLSID png = PngEncoder();
                 Check(image.Save((out_ / L"12-hint.png").c_str(), &png, nullptr) == Gdiplus::Ok, "screenshot 12-hint");
             }
             DeleteObject(bmp);
             DeleteDC(dc);
             ReleaseDC(nullptr, screen);
             Tip::Hide();
             return true;
         }},    };
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
