#pragma once

#include "common.h"

#include <set>

// One open file: a Scintilla control plus file metadata and diagnostics.
class Editor {
public:
    static constexpr int kIndicatorError = 8;
    static constexpr int kIndicatorWarning = 9;
    static constexpr int kIndicatorFind = 10;
    static constexpr int kIndicatorLink = 11;  // Ctrl+hover underline
    static constexpr int kMarkerError = 1;       // line background
    static constexpr int kMarkerWarning = 2;     // line background
    static constexpr int kMarkerBreakpoint = 3;  // red dot in the margin
    static constexpr int kMarkerExecArrow = 4;   // current execution line (margin)
    static constexpr int kMarkerExecLine = 5;    // current execution line (background)

    Editor(HWND parent, int fontSize);
    ~Editor();
    Editor(const Editor&) = delete;
    Editor& operator=(const Editor&) = delete;

    sptr_t Call(unsigned msg, uptr_t w = 0, sptr_t l = 0) const { return fn_(ptr_, msg, w, l); }

    HWND hwnd() const { return hwnd_; }
    int id() const { return id_; }
    const fs::path& file() const { return file_; }
    void setFile(const fs::path& f) { file_ = f; }
    int version() const { return version_; }
    void bumpVersion() { version_++; }

    std::wstring Title() const;
    bool Modified() const { return Call(SCI_GETMODIFY) != 0; }
    std::string Text() const;
    std::wstring EncodingName() const { return ansi_ ? L"windows-1251" : (bom_ ? L"UTF-8 BOM" : L"UTF-8"); }
    std::wstring EolName() const;
    int CaretLine() const;
    int CaretColumn() const;

    bool Load(const fs::path& path, std::wstring& error);
    bool Save(const fs::path& path, std::wstring& error);
    void SetText(const std::string& utf8);

    void ApplyStyles(int fontSize);
    void Lex(Sci_Position endPos);  // container lexer, called on SCN_STYLENEEDED

    void SetDiagnostics(const std::vector<Diagnostic>& list);
    const std::vector<Diagnostic>* DiagnosticsAt(int line) const;
    int Count(Severity s) const;

    void OnCharAdded(int ch);
    void ToggleComment();
    void GotoLine(int line);
    void ShowDiagnosticTip(Sci_Position pos);

    std::set<int> Breakpoints() const;  // 1-based lines
    void SetBreakpoints(const std::set<int>& lines);
    void ToggleBreakpoint(int line);
    void SetExecutionLine(int line);    // 0 clears
    std::string WordAt(Sci_Position pos) const;
    bool WordRange(Sci_Position pos, Sci_Position& start, Sci_Position& end) const;

private:
    HWND hwnd_;
    SciFnDirect fn_;
    sptr_t ptr_;
    int id_;
    int untitled_ = 0;
    int version_ = 0;
    fs::path file_;
    bool bom_ = false;
    bool ansi_ = false;
    std::map<int, std::vector<Diagnostic>> diagnostics_;
    std::vector<Token> tokens_;

    std::string LineText(Sci_Position line) const;
    void RegisterCompletionIcons();
    void SelectedLines(Sci_Position& first, Sci_Position& last) const;
};
