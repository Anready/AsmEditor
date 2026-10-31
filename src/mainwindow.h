#pragma once

#include "common.h"
#include "completion.h"
#include "debugger.h"
#include "editor.h"
#include "terminal.h"
#include "update.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <thread>

class UiTest;

class MainWindow {
public:
    MainWindow();
    ~MainWindow();
    bool Create(HINSTANCE inst, Settings* settings);
    HWND hwnd() const { return hwnd_; }
    bool PreTranslate(MSG& msg);
    void OpenPath(const fs::path& path);
    void RestoreSession();
    void StartUiTest(const fs::path& out);  // --ui-test: see uitest.cpp

private:
    friend class UiTest;
    void UiTestTick();
    std::unique_ptr<UiTest> uiTest_;
    int uiTestExit_ = 0;
    bool compactTools_ = false;  // narrow window: toolbar buttons show icons only

    struct ToolItem {
        int id;
        const wchar_t* text;  // Russian; shown through Tr
        int icon;      // see DrawIcon
        bool menu;
        bool right;
        int show = 0;  // 0 always, 1 only while debugging, 2 only when not debugging
        RECT rc{};
    };
    enum class After { Nothing, Run, Debug, DebugEntry, RunToCursor, Profile, Sample };
    using Runs = std::vector<std::pair<std::string, int>>;  // text + panel style
    struct CheckJob {
        int editorId;
        int version;
        std::string text;
        std::vector<fs::path> includes;
        Settings settings;
        RunConfig config;
    };
    enum class Drag { None, Tree, Console, Debug, Tab, Split };
    // Tabs of the bottom panel. The terminal shows only the program's console; messages go to the others.
    enum class Pane { Output, Terminal, Problems, Search, DebugOutput, Count };
    enum class LeftTab { Project, Outline };
    enum class DebugTab { Registers, Stack, Watch, Disasm, Flags, Count };
    enum class MemFormat { Bytes, Words, Dwords, Qwords, Float, Double, Text, Count };

    // window plumbing
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK InputProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data);
    static LRESULT CALLBACK ConsoleProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data);
    static LRESULT CALLBACK MarkBarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    void CreateChildren();
    void CreateFonts();
    void BuildMenus();
    void Layout();
    void SplitGeometry(const RECT& area, bool split, bool down, RECT& tabs1, RECT& editor1, RECT& gap, RECT& tabs2,
                       RECT& editor2) const;
    void Paint(HDC dc);
    void PaintToolbar(HDC dc);
    void PaintTabs(HDC dc, int group);
    void ProjectMenu(const RECT& anchor);
    void OpenToolMenu(int index, bool byHover = false);
    bool MouseInToolMenu() const;
    static LRESULT CALLBACK MenuFilter(int code, WPARAM wp, LPARAM lp);
    void PaintStatus(HDC dc);
    void PaintWelcome(HDC dc);
    void PaintHeaderTabs(HDC dc, const RECT& area, const std::vector<std::wstring>& titles, int active,
                         std::vector<RECT>& rects, int hover);
    void DrawIcon(HDC dc, int icon, const RECT& rc, bool enabled);
    std::wstring ToolLabel(const ToolItem& t) const;  // text of a toolbar item (drop-downs show their value)
    void OnMouseMove(int x, int y);
    void OnClick(int x, int y, bool middle);
    void OnRightClick(int x, int y);
    void OnCommand(int id);
    void OnNotify(NMHDR* nm);
    void UpdateTitle();
    void InvalidateChrome();
    int S(int v) const { return Scale(v, dpi_); }

    // commands: names, shortcuts (customizable), menus and the command palette
    struct CommandDef {
        int id;
        const wchar_t* name;  // Russian; shown through Tr
        const wchar_t* keys;  // default shortcut, "" for none
    };
    static const std::vector<CommandDef>& Commands();
    std::wstring KeysFor(int id) const;
    std::wstring MenuText(int id) const;
    void AppendCommand(HMENU menu, int id);
    void AppendCommands(HMENU menu, std::initializer_list<int> ids);
    void AppendSubmenu(HMENU menu, const wchar_t* title, std::initializer_list<int> ids);
    void BuildAccelerators();
    void CommandPalette();
    void QuickOpen();
    void EditKeys();
    void ToggleAssociation();
    void ShowHelpPage();
    // updates of AsmEditor (mw_update.cpp)
    void MaybeCheckForUpdate();
    void CheckForAppUpdate(bool interactive);
    void OnUpdateInfo(LPARAM lp);
    void InstallUpdate(const Update::Release& r);
    void OnUpdateDone(LPARAM lp);
    std::optional<Update::Release> pendingRelease_;  // found by the check at start
    bool restartAfterClose_ = false;
public:
    void RestartIfUpdated();  // after the message loop: start the replaced exe
private:  // F1: a page with the shortcuts and short examples, opened in the browser  // Help → open .asm files in AsmEditor (HKCU)

    // editors
    Editor* Active() const { return active_ >= 0 && active_ < (int)editors_.size() ? editors_[active_].get() : nullptr; }
    Editor* FindByHwnd(HWND h) const;
    Editor* FindById(int id) const;
    int FindByPath(const fs::path& p) const;
    int IndexOf(const Editor* ed) const;
    void AddEditor(std::unique_ptr<Editor> ed);
    void Activate(int index);
    bool ConfirmClose(int index);
    void CloseEditor(int index);
    bool CloseAll();
    void CloseOthers(int keep);
    Editor* OpenFile(const fs::path& path, int line = 0);
    bool Save(Editor* ed);
    bool SaveAs(Editor* ed);
    void SaveAll();
    void ApplyFontSize(int size);
    void RestyleAll();
    void ApplyTheme(const std::wstring& id);
    void Relocalize();
    void TabMenu(int index);
    void TogglePin(int index);
    void MoveTab(int from, int to);
    void SortPinnedTabs();
    void SplitWith(int index, bool down = false);
    void Unsplit(int primary = 0);
    Editor* SplitEditor() const { return splitIndex_ >= 0 && splitIndex_ < (int)editors_.size() ? editors_[splitIndex_].get() : nullptr; }
    Editor* Current() const;  // the editor commands act on: the focused one, also in a detached window
    std::vector<Editor*> PanelViews() const;

    // Tab groups (mw_float.cpp). Every window is made of one or two tab groups (the second right of or below the
    // first), and any group holds files and parts of the debugger alike: the main editor (groups 0 and 1), the
    // debug panel on its right (2 and 3) and windows of their own, e.g. on a second monitor (>= 4). Each group has
    // a tab row and shows one tab: a part (shownPart_) or else a file (active_ for group 0, splitIndex_ for group
    // 1, FloatGroup::shown for the others).
    static constexpr int kPanelGroup = 2, kPanelGroup2 = 3, kFirstFloatGroup = 4;
    // Parts of the debugger: the views of DebugTab and the memory view.
    static constexpr int kMemoryPart = static_cast<int>(DebugTab::Count);
    static constexpr int kDebugParts = kMemoryPart + 1;
    struct FloatGroup {  // a group of the debug panel or of a window of its own
        HWND hwnd = nullptr;
        int shown = 0;        // id of the file shown
        int partner = 0;      // (first half) the second half (0: not split)
        bool second = false;  // this group is the second half
        bool down = false;    // (first half) the second half is below, not to the right
        RECT island{}, rcTabs{}, rcEditor{}, rcSplit{};
    };
    struct TabItem {  // what a tab shows: a file (editor index) or a part of the debugger
        int editor = -1;
        int part = -1;
    };
    FloatGroup* GroupInfo(int group);
    const FloatGroup* GroupInfo(int group) const;
    int WindowPrimary(int group) const;  // the first group of the window that holds `group` (main: 0, panel: 2)
    int SecondHalf(int primary) const;   // its second group (0: not split)
    bool IsSecondHalf(int group) const;
    int EnsureSecondHalf(int primary, bool down);
    static LRESULT CALLBACK FloatProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    HWND CreateFloat(const std::wstring& title, RECT screen);
    void DetachEditor(int index, const POINT* at);  // at: the cursor (screen) when a tab is dragged out
    RECT DetachedRect(const POINT* at) const;       // where a new window goes (screen)
    void DockEditor(Editor* ed);
    HWND FloatOf(const Editor* ed) const;
    bool IsFloating(const Editor* ed) const { return ed && ed->group >= kFirstFloatGroup; }
    bool IsFloatWindow(HWND root) const;
    int FloatGroupOf(HWND f) const;  // the first group of a window of its own (0: not one)
    bool WindowShown(int primary) const;
    bool PanelShown() const;
    void UpdateFloatTitles();
    std::vector<int> GroupTabs(int group) const;  // file indices in tab order
    std::vector<int> GroupParts(int group) const;
    bool GroupHasItems(int group) const;
    int GroupItemCount(int group) const;
    int ItemGroup(TabItem item) const { return item.part >= 0 ? partGroup_[item.part] : editors_[item.editor]->group; }
    HWND GroupHost(int group) const;
    RECT StripRect(int group) const;     // the tab row (client of GroupHost)
    RECT GroupContent(int group) const;  // below the tab row
    RECT GroupArea(int group) const;     // both
    int ShownIndex(int group) const;
    int ShownPart(int group) const;      // -1: the group shows a file
    int NeighbourInGroup(int index) const;  // the nearest other file of the same group (-1: none)
    void MoveToGroup(int index, int group);
    void MoveItem(TabItem item, int group);
    void SplitItem(TabItem item, int primary, bool down);
    void DetachItem(TabItem item, const POINT* at);
    void MoveWindowItems(int primary, int target);  // every tab of a window (both halves) into `target`
    void FixGroups();  // after changes: shown tabs, emptied halves and windows, parents
    bool NoDockedEditors() const;  // the main editor has no tabs (the welcome page shows)
    void PaintFloat(HDC dc, int group);
    int TabAt(HWND host, int x, int y, bool* close = nullptr) const;
    int PartTabAt(HWND host, int x, int y) const;
    void InvalidateTabs();
    bool TabMouseDown(HWND host, int x, int y);
    void TabDragMove(HWND host, int x, int y);
    void TabDrop(HWND host, int x, int y);
    void FloatMoving(HWND f, bool done);  // a detached window dragged by its title: over the main one it docks
    bool DockTargetAt(HWND f, POINT screen, RECT& hint, int& group) const;
    RECT PanelDropStrip() const;  // where the hidden debug panel would be (client): parts can be dropped there
    void ActivatePart(int part, bool focus = true);
    HWND PartHost(int part) const { return GroupHost(partGroup_[part]); }
    Editor* PartView(int part) const;
    std::wstring PartTitle(int part) const;
    void PlacePart(int part, const RECT& content);
    void HidePart(int part);
    void PaintPartChrome(HDC dc, HWND host);  // the memory format and the input frames shown in `host`
    bool MemFormatClick(HWND host, int x, int y);
    void MemFormatHover(HWND host, int x, int y);
    void PartMenu(int part);
    void ResetDebugParts();  // the panel's tabs above the memory view, as at first
    void ToggleDebugWindow();
    void SaveDebugLayout();
    void RestoreDebugLayout();
    enum class Drop { None, Right, Down, Window, Group };
    Drop TabDropZone(POINT screen, RECT& hint, int& group) const;
    Drop ZoneIn(int primary, POINT client, int from, RECT& hint, int& group) const;
    void ShowDropHint(const RECT* screen);

    // project
    bool OpenProject(const fs::path& dir);
    void CloseProject();
    void SaveProjectSession();
    void NewProject();
    void SetMainFile(const fs::path& f);
    void RefreshTree();
    void BuildTree(HTREEITEM parent, const fs::path& dir, int depth);
    fs::path TreePath(HTREEITEM item) const;
    void TreeContextMenu();
    void AddRecent(const fs::path& dir);
    std::optional<fs::path> PickFolder(const wchar_t* title);
    std::vector<fs::path> ProgramFiles();  // project files with their own main
    std::vector<fs::path> ProjectFiles();  // every file of the project tree
    void ShowLeftTab(LeftTab tab);
    void RefreshOutline();
    // external changes, autosave and crash recovery
    void CheckExternalChanges();
    fs::path BackupDir() const;
    void WriteBackups();
    void RemoveBackup(const Editor* ed);
    void OfferRecovery();

    // build & run
    std::vector<fs::path> IncludeDirs(const fs::path& file) const;
    fs::path BuildTarget();
    void StartBuild(After after, bool rebuild = false);
    void OnBuildDone(BuildResult* r, After after, const fs::path& src);
    void RunProgram(const fs::path& exe, const fs::path& cwd, After mode);
    void RunExternal(const fs::path& exe, const fs::path& cwd);
    void RunInWsl(const fs::path& exe, const fs::path& cwd, bool debug);
    void StopProgram();
    void OnProgramExit(DWORD code, DWORD generation);
    fs::path RunDirectory(const fs::path& src) const;
    LaunchOptions MakeLaunch(const fs::path& exe, const fs::path& cwd) const;
    void FeedStdin();
    void EditRunConfig();
    void ChooseTarget();
    void ChooseProfile();
    void CheckSolution();
    void NewExercise();
    void OpenLesson();
    void LoadTeacherTasks();  // Run → load a task file (kept in the settings folder, loaded at start)
    void ExerciseHint();
    void GroupSummary();
    std::map<std::string, size_t> hintsShown_;  // exercise -> hints given
    void ExportResults();
    void RecordResult(const std::string& id, int passed, int total);
    static fs::path ResultsFile();
    void CheckUpdates();
    // program console (ConPTY)
    bool StartConsole();
    void EndConsole();
    void FinishConsole(DWORD exitCode);
    void OnConsoleDone(DWORD exitCode);
    void RenderTerminal();
    void SendToProgram(const std::string& bytes);
    bool ConsoleKey(UINT msg, WPARAM wp);
    std::pair<int, int> ConsoleGridSize() const;
    void TrimConsole();
    void Print(const std::string& utf8, Out style);  // to the Output tab
    void PrintTo(Pane pane, const std::string& utf8, Out style);
    void ClearPane(Pane pane);
    void ShowPane(Pane pane);
    Editor& View(Pane pane) const { return *views_[static_cast<int>(pane)]; }
    void SetupOutputView(Editor& view);
    void RefreshProblems();
    void ConsoleDoubleClick(Editor& view, Sci_Position pos);
    void RequestCheck(Editor* ed);
    void CheckWorker();
    void RefreshToolStatus();
    UINT ProgramCodePage() const;

    // debugging
    bool DebugStopped() const { return debugger_ && debugger_->Stopped(); }
    void DebugCommand(int id);
    void OnDebugStopped(std::unique_ptr<DebugStop> stop);
    void OnDebugExited(DWORD code, const std::string& stats);
    void SetDebugging(bool on);
    void ClearExecutionLine();
    void ToggleBreakpoint(Editor* ed, int line);
    void EditBreakpointCondition(Editor* ed, int line, bool logpoint = false);
    void BreakpointMenu(Editor* ed, int line);
    void StashBreakpoints(Editor* ed);
    Debugger::Breakpoints CollectBreakpoints() const;
    void RunToCursor();
    void SetNextStatement();
    void RenderPanel(Editor& view, const Runs& runs);
    void RenderDebugPanel();  // the visible tab and the memory view
    void RenderRegisters();
    void RenderCallStack();
    void RenderWatches();
    std::string ProjectSourceText();  // open files and the project's .asm/.inc (structures for the watches)
    void RenderDisassembly();
    void RenderMemory();
    void RenderFlags();
    void StepBack();
    void AttachToProcess();
    void OnProfile(std::vector<ProfileLine> lines);
    void ShowProfileNotes(Editor* ed);
    void ClearProfile();
    void ShowDebugTab(DebugTab tab);
    void PanelDoubleClick(Editor& view, Sci_Position pos);
    void EditRegister(const std::string& name);
    void EditMemory(uint64_t address);
    void AddWatch(const std::string& expression);
    void AddDataBreakpoint(const std::string& expression);
    std::string DebugTipText(Editor* ed, Sci_Position pos);
    void ShowHint(Editor* ed, Sci_Position pos);
    std::string SymbolHint(Editor* ed, const std::string& word);
    void ShowSignature(Editor* ed);
    void SetupPanelView(Editor& view);
    bool DebugPanelVisible() const { return debugging_ || showDebugPanel_; }

    // completion, snippets, formatting, multiple carets
    void Complete(Editor* ed, bool explicitRequest);
    std::vector<Completion::Symbol> OtherFileSymbols(const Editor* current);
    void ShowSnippets(Editor* ed);
    void InsertSnippet(Editor* ed, const std::string& name);
    void FormatCode(Editor* ed);
    void AddCaret(Editor* ed, bool down);
    void HighlightOccurrences(Editor* ed);
    void UpdateBracePairs(Editor* ed);
    void FoldAll(Editor* ed, bool fold);
    void QuickFix(Editor* ed);                           // Alt+Enter: fixes for the problems on the caret line
    void ShowCallArguments(Editor* ed);                  // while typing "call name": its arguments
    std::string CallArgumentsText(Editor* ed, int line); // which argument registers are set before a call

    // find
    void ShowFind(bool replace);
    void OnFindMessage(FINDREPLACEW* fr);
    bool FindNext(bool forward);
    void FindInFiles();

    // mark bar: errors, warnings, breakpoints and search hits along the vertical scrollbar
    void PaintMarkBar(HWND bar, HDC dc);
    void MarkBarClick(HWND bar, int y);

    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND tree_ = nullptr;
    HWND outline_ = nullptr;
    HWND markBar_ = nullptr, markBar2_ = nullptr;
    std::unique_ptr<Editor> views_[static_cast<int>(Pane::Count)];  // bottom panel
    Pane pane_ = Pane::Output;
    std::vector<RECT> paneRects_, leftTabRects_;
    int hoverPane_ = -1, hoverLeftTab_ = -1;
    LeftTab leftTab_ = LeftTab::Project;
    std::vector<int> outlineLines_;  // outline item lParam -> line
    int outlineEditor_ = 0;          // editor id the outline shows
    HACCEL accel_ = nullptr;
    HMENU fileMenu_ = nullptr, editMenu_ = nullptr, viewMenu_ = nullptr, runMenu_ = nullptr, recentMenu_ = nullptr;
    HMENU helpMenu_ = nullptr, themeMenu_ = nullptr;
    HFONT uiFont_ = nullptr, uiBold_ = nullptr, titleFont_ = nullptr;
    HBRUSH inputBrush_ = nullptr;
    int dpi_ = 96;
    Settings* settings_ = nullptr;

    std::vector<std::unique_ptr<Editor>> editors_;
    int active_ = -1;
    int splitIndex_ = -1;     // editor shown in the right half (-1: not split)
    bool focusRight_ = false; // the right half has the focus: tabs open there
    bool splitDown_ = false;  // the second editor is below the first one, not to the right
    std::map<int, FloatGroup> floatGroups_;  // group (>= kFirstFloatGroup) -> its window
    FloatGroup panel_[2];                    // the debug panel's groups (kPanelGroup, kPanelGroup2)
    bool panelDown_ = true;                  // its second group is below the first one
    int nextGroup_ = kFirstFloatGroup;
    int floatFocus_ = 0;              // a group other than the main editor's that has the focus (0: none)
    HWND dragHost_ = nullptr;         // window a tab is dragged from
    bool floatMoved_ = false;         // a detached window is being moved (not resized) by its title
    HWND dropHint_ = nullptr;         // translucent rectangle showing where a dragged tab goes
    // The group of each part (at first the panel: its tabs above the memory view) and the part a group shows.
    int partGroup_[kDebugParts] = {kPanelGroup, kPanelGroup, kPanelGroup, kPanelGroup, kPanelGroup, kPanelGroup2};
    std::map<int, int> shownPart_;
    RECT partTabRects_[kDebugParts]{};
    int hoverPart_ = -1, dragPart_ = -1;
    std::vector<ToolItem> tools_;
    std::vector<RECT> tabRects_, closeRects_;
    std::vector<std::pair<RECT, std::wstring>> welcomeLinks_;
    std::map<int, int> tabScroll_;     // group -> scroll of its tab row
    std::set<int> ensureTabVisible_;   // groups whose shown tab is scrolled into view at the next paint
    int hoverTool_ = -1, hoverTab_ = -1, hoverClose_ = -1, hoverLink_ = -1;
    int openMenuTool_ = -1, nextMenuTool_ = -1;  // the open title menu, the one to open after it
    int menuAwayTicks_ = 0;                       // 100 ms ticks the mouse has spent away from a hover-opened menu
    bool hoverClear_ = false, hoverRefresh_ = false, hoverMemFormat_ = false;
    Drag drag_ = Drag::None;
    int dragTab_ = -1, dragStartX_ = 0, dragStartY_ = 0;
    bool dragMoved_ = false;

    RECT rcToolbar_{}, rcStatus_{}, rcTreeHeader_{}, rcTabs_{}, rcTabs2_{}, rcEditor_{}, rcEditor2_{}, rcConsoleHeader_{};
    RECT rcSplitV_{}, rcSplitH_{}, rcSplitE_{}, rcClear_{}, rcRefresh_{};
    std::vector<RECT> islands_;  // the rounded panels, painted on Theme::FrameBg
    RECT rcDebug_{}, rcSplitD_{}, rcMemRow_{}, rcWatchRow_{}, rcMemFormat_{};  // the inputs: in their part's window

    std::unique_ptr<Debugger> debugger_;
    bool debugging_ = false;
    bool showDebugPanel_ = false;
    std::unique_ptr<Editor> regsView_, memView_, stackView_, watchView_, disasmView_, flagsView_;
    HWND memExpr_ = nullptr, watchExpr_ = nullptr;
    MemFormat memFormat_ = MemFormat::Bytes;
    std::vector<uint8_t> prevMem_;    // bytes shown at the previous stop, to highlight changes
    uint64_t prevMemAddress_ = 0;
    std::vector<std::string> watches_;  // expressions of the watch list
    std::vector<std::string> regLines_;      // register name per line of the registers view
    std::vector<uint64_t> memLines_;         // address per line of the memory view
    std::vector<int> stackLines_;            // frame per line of the call stack view (-1: none)
    std::vector<int> watchLines_;            // watch (>= 0) or data breakpoint (-2 - index) per line
    std::vector<int> flagLines_;             // flag bit per line of the flags view (-1: none)
    std::vector<uint64_t> disasmLines_;      // address per line of the disassembly
    std::vector<Debugger::Frame> frames_;
    std::wstring debugStatus_;
    COLORREF debugStatusColor_ = 0;
    fs::path execFile_;
    alignas(16) CONTEXT lastContext_{};
    bool haveLastContext_ = false;
    std::vector<ProfileLine> profile_;          // last profiler run (notes in the editors)
    std::vector<uint8_t> prevStack_;            // stack bytes at the previous stop (for the stack animation)
    uint64_t prevStackRsp_ = 0;
    int stackFlash_ = 0;                        // frames left of the fading highlight of changed stack slots
    std::optional<SourceLocation> runTo_;    // "run to cursor" requested before the program was started
    Debugger::Breakpoints breakpoints_;  // Debugger::Key(path) -> lines, for files not open
    std::set<std::wstring> excludedFromBuild_;  // Project::Key of files the user excluded from the build
    struct CachedSymbols {
        fs::file_time_type time;
        std::vector<Completion::Symbol> symbols;
        fs::path path;
    };
    std::map<std::wstring, CachedSymbols> symbolCache_;  // project files that are not open

    // go to definition
    struct SymbolHit {
        Editor* editor;  // open file (or nullptr)
        fs::path file;
        Completion::Symbol symbol;
    };
    struct NavLocation {
        fs::path file;
        int editorId;
        Sci_Position pos;
    };
    std::vector<SymbolHit> ProjectSymbols(const Editor* current);
    void GoToDefinition(Editor* ed, Sci_Position pos);
    bool OpenIncludeAt(Editor* ed, Sci_Position pos);
    void JumpTo(const SymbolHit& hit);
    NavLocation CurrentLocation();
    void PushNavigation();
    void Navigate(bool back);
    void UpdateLinkHover(Editor* ed, bool ctrl);
    void ClearLinkHover();
    struct UsageSource {
        fs::path file;
        Editor* editor;  // open tab, or nullptr for a file read from disk
        std::string text;
    };
    bool SymbolUnderCaret(Editor* ed, Sci_Position pos, std::string& word, std::string& scope, bool quiet = false);
    std::vector<UsageSource> UsageSources(Editor* ed, bool local);
    void FindUsages(Editor* ed, Sci_Position pos);
    void Rename(Editor* ed, Sci_Position pos);
    void ClearUsageHighlight();
    static LRESULT CALLBACK EditorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data);
    std::vector<NavLocation> navBack_, navForward_;
    Editor* linkEditor_ = nullptr;  // editor showing the Ctrl+hover underline
    Sci_Position linkStart_ = 0, linkEnd_ = 0;
    Editor* usageEditor_ = nullptr;  // editor with highlighted usages
    std::string occurrenceWord_;     // word whose occurrences are highlighted
    int occurrenceEditor_ = 0;

    fs::path projectRoot_;
    fs::path mainFile_;
    RunConfig runConfig_;
    std::vector<fs::path> treePaths_;
    HTREEITEM contextItem_ = nullptr;

    bool building_ = false;
    HANDLE process_ = nullptr, job_ = nullptr;
    DWORD generation_ = 0;
    ULONGLONG runStartTick_ = 0;
    std::string exitStats_;
    std::vector<Diagnostic> buildDiagnostics_;  // of the last build, for the Problems tab
    int problemCount_ = 0;                      // distinct problems listed there
    struct SolutionCheck {
        std::string exercise;
        fs::path exe;
    };
    std::optional<SolutionCheck> pendingCheck_;

    HPCON pty_ = nullptr;           // pseudoconsole of the running program
    HANDLE ptyInput_ = nullptr;     // keyboard input to the program
    std::thread ptyReader_;
    Terminal term_;
    bool termActive_ = false;       // the end of the terminal view mirrors the terminal screen
    Sci_Position termStart_ = 0;    // where the terminal screen starts in the terminal view
    DWORD consoleSession_ = 0;
    bool renderPending_ = false;
    wchar_t pendingSurrogate_ = 0;
    std::wstring toolStatus_;
    bool toolStatusWarn_ = false;

    std::thread checkThread_;
    std::mutex checkMutex_;
    std::condition_variable checkCv_;
    std::deque<CheckJob> checkQueue_;
    bool checkStop_ = false;

    HWND findDialog_ = nullptr;
    UINT findMsg_ = 0;
    FINDREPLACEW findData_{};
    wchar_t findText_[256] = L"";
    wchar_t replaceText_[256] = L"";
    std::wstring lastFind_;
    DWORD lastFindFlags_ = FR_DOWN;
    FindInFilesRequest lastFindInFiles_;
};
