#include "editor.h"

#include "completion.h"

#include <cmath>

#include <fstream>
#include <sstream>

namespace {
std::atomic<int> g_nextId{1};
std::atomic<int> g_nextUntitled{1};

void StyleColors(const Editor& e, int style, COLORREF fore, bool italic = false, bool bold = false) {
    e.Call(SCI_STYLESETFORE, style, fore);
    e.Call(SCI_STYLESETBACK, style, Theme::EditorBg);
    e.Call(SCI_STYLESETITALIC, style, italic);
    e.Call(SCI_STYLESETBOLD, style, bold);
}

// Scintilla element colours take 0xAABBGGRR.
sptr_t Opaque(COLORREF c, int alpha = 0xFF) {
    return static_cast<sptr_t>(c) | (static_cast<sptr_t>(alpha) << 24);
}
}

Editor::Editor(HWND parent, int fontSize) : id_(g_nextId++) {
    hwnd_ = CreateWindowExW(0, L"Scintilla", L"", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 10, 10, parent, nullptr,
                            GetModuleHandleW(nullptr), nullptr);
    fn_ = reinterpret_cast<SciFnDirect>(SendMessageW(hwnd_, SCI_GETDIRECTFUNCTION, 0, 0));
    ptr_ = static_cast<sptr_t>(SendMessageW(hwnd_, SCI_GETDIRECTPOINTER, 0, 0));
    ApplyDarkControl(hwnd_);

    Call(SCI_SETCODEPAGE, SC_CP_UTF8);
    Call(SCI_SETMODEVENTMASK, SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT);
    Call(SCI_SETTABWIDTH, 4);
    Call(SCI_SETUSETABS, 0);
    Call(SCI_SETINDENT, 4);
    Call(SCI_SETTABINDENTS, 1);
    Call(SCI_SETBACKSPACEUNINDENTS, 1);
    Call(SCI_SETSCROLLWIDTHTRACKING, 1);
    Call(SCI_SETSCROLLWIDTH, 1);
    Call(SCI_SETMOUSEDWELLTIME, 450);
    Call(SCI_SETMARGINS, 3);
    Call(SCI_SETMARGINTYPEN, 0, SC_MARGIN_SYMBOL);  // breakpoints and the execution arrow
    Call(SCI_SETMARGINMASKN, 0, (1 << kMarkerBreakpoint) | (1 << kMarkerExecArrow));
    Call(SCI_SETMARGINSENSITIVEN, 0, 1);
    Call(SCI_SETMARGINCURSORN, 0, SC_CURSORARROW);
    Call(SCI_SETMARGINTYPEN, 1, SC_MARGIN_NUMBER);
    Call(SCI_SETMARGINMASKN, 1, 0);
    Call(SCI_SETMARGINTYPEN, 2, SC_MARGIN_COLOUR);
    Call(SCI_SETMARGINMASKN, 2, 0);
    Call(SCI_SETMARGINWIDTHN, 2, 8);
    Call(SCI_SETCARETLINEVISIBLEALWAYS, 1);
    Call(SCI_SETCARETWIDTH, 2);
    Call(SCI_SETEXTRADESCENT, 2);
    Call(SCI_SETENDATLASTLINE, 0);
    Call(SCI_USEPOPUP, SC_POPUP_TEXT);
    Call(SCI_ASSIGNCMDKEY, 'D' + (SCMOD_CTRL << 16), SCI_SELECTIONDUPLICATE);

    Call(SCI_MARKERDEFINE, kMarkerError, SC_MARK_BACKGROUND);
    Call(SCI_MARKERSETBACK, kMarkerError, Hex(0x3A2427));
    Call(SCI_MARKERDEFINE, kMarkerWarning, SC_MARK_BACKGROUND);
    Call(SCI_MARKERSETBACK, kMarkerWarning, Hex(0x37321F));
    Call(SCI_MARKERDEFINE, kMarkerBreakpoint, SC_MARK_CIRCLE);
    Call(SCI_MARKERSETFORE, kMarkerBreakpoint, Hex(0xDB5C5C));
    Call(SCI_MARKERSETBACK, kMarkerBreakpoint, Hex(0xDB5C5C));
    Call(SCI_MARKERDEFINE, kMarkerExecArrow, SC_MARK_SHORTARROW);
    Call(SCI_MARKERSETFORE, kMarkerExecArrow, Hex(0x1E1F22));
    Call(SCI_MARKERSETBACK, kMarkerExecArrow, Hex(0xF2C55C));
    Call(SCI_MARKERDEFINE, kMarkerExecLine, SC_MARK_BACKGROUND);
    Call(SCI_MARKERSETBACK, kMarkerExecLine, Hex(0x3B3520));

    Call(SCI_INDICSETSTYLE, kIndicatorError, INDIC_SQUIGGLEPIXMAP);
    Call(SCI_INDICSETFORE, kIndicatorError, Theme::Error);
    Call(SCI_INDICSETSTYLE, kIndicatorWarning, INDIC_SQUIGGLEPIXMAP);
    Call(SCI_INDICSETFORE, kIndicatorWarning, Theme::Warning);
    Call(SCI_INDICSETSTYLE, kIndicatorFind, INDIC_ROUNDBOX);
    Call(SCI_INDICSETFORE, kIndicatorFind, Theme::Accent);
    Call(SCI_INDICSETALPHA, kIndicatorFind, 60);
    Call(SCI_INDICSETSTYLE, kIndicatorLink, INDIC_PLAIN);
    Call(SCI_INDICSETFORE, kIndicatorLink, Theme::Link);

    // Completion list: we filter and sort (case-insensitively) ourselves; "word?kind" picks the icon.
    Call(SCI_AUTOCSETTYPESEPARATOR, '?');
    Call(SCI_AUTOCSETIGNORECASE, 1);
    Call(SCI_AUTOCSETCASEINSENSITIVEBEHAVIOUR, SC_CASEINSENSITIVEBEHAVIOUR_IGNORECASE);
    Call(SCI_AUTOCSETORDER, SC_ORDER_PRESORTED);
    Call(SCI_AUTOCSETMAXHEIGHT, 12);
    Call(SCI_AUTOCSETMAXWIDTH, 40);
    Call(SCI_AUTOCSETDROPRESTOFWORD, 1);
    Call(SCI_SETELEMENTCOLOUR, SC_ELEMENT_LIST, Opaque(Theme::Text));
    Call(SCI_SETELEMENTCOLOUR, SC_ELEMENT_LIST_BACK, Opaque(Hex(0x2B2D30)));
    Call(SCI_SETELEMENTCOLOUR, SC_ELEMENT_LIST_SELECTED, Opaque(RGB(255, 255, 255)));
    Call(SCI_SETELEMENTCOLOUR, SC_ELEMENT_LIST_SELECTED_BACK, Opaque(Hex(0x2E436E)));
    RegisterCompletionIcons();

    ApplyStyles(fontSize);
    Call(SCI_EMPTYUNDOBUFFER);
    Call(SCI_SETSAVEPOINT);
}

// Small rounded squares in the color of each completion kind.
void Editor::RegisterCompletionIcons() {
    int size = std::max(10, MulDiv(10, static_cast<int>(GetDpiForWindow(hwnd_)), 96));
    Call(SCI_RGBAIMAGESETWIDTH, size);
    Call(SCI_RGBAIMAGESETHEIGHT, size);
    std::vector<uint8_t> pixels(static_cast<size_t>(size) * size * 4);
    for (int kind = 1; kind <= static_cast<int>(Completion::Kind::Section); kind++) {
        COLORREF c = Completion::KindColor(static_cast<Completion::Kind>(kind));
        double r = size / 4.0;
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) {
                // distance outside the rounded rectangle, for a soft edge
                double dx = std::max({r - x - 0.5, x + 0.5 - (size - r), 0.0});
                double dy = std::max({r - y - 0.5, y + 0.5 - (size - r), 0.0});
                double alpha = std::clamp(r + 0.5 - std::sqrt(dx * dx + dy * dy), 0.0, 1.0);
                uint8_t* p = &pixels[(static_cast<size_t>(y) * size + x) * 4];
                p[0] = GetRValue(c);
                p[1] = GetGValue(c);
                p[2] = GetBValue(c);
                p[3] = static_cast<uint8_t>(alpha * 255);
            }
        }
        Call(SCI_REGISTERRGBAIMAGE, kind, reinterpret_cast<sptr_t>(pixels.data()));
    }
}

Editor::~Editor() {
    DestroyWindow(hwnd_);
}

void Editor::ApplyStyles(int fontSize) {
    std::string font = Narrow(MonospaceFontName());
    Call(SCI_STYLESETFONT, STYLE_DEFAULT, reinterpret_cast<sptr_t>(font.c_str()));
    Call(SCI_STYLESETSIZE, STYLE_DEFAULT, fontSize);
    Call(SCI_STYLESETFORE, STYLE_DEFAULT, Theme::Text);
    Call(SCI_STYLESETBACK, STYLE_DEFAULT, Theme::EditorBg);
    Call(SCI_STYLECLEARALL);

    StyleColors(*this, T_DEFAULT, Theme::Text);
    StyleColors(*this, T_COMMENT, Hex(0x7A7E85), true);
    StyleColors(*this, T_STRING, Hex(0x6AAB73));
    StyleColors(*this, T_NUMBER, Hex(0x2AACB8));
    StyleColors(*this, T_REGISTER, Hex(0xC77DBB));
    StyleColors(*this, T_INSTRUCTION, Hex(0xCF8E6D));
    StyleColors(*this, T_DIRECTIVE, Hex(0x56A8F5));
    StyleColors(*this, T_PREPROCESSOR, Hex(0xB3AE60));
    StyleColors(*this, T_LABEL, Hex(0xFFC66D));
    StyleColors(*this, T_KEYWORD, Hex(0xE0957B));

    Call(SCI_STYLESETFORE, STYLE_LINENUMBER, Hex(0x4B5059));
    Call(SCI_STYLESETBACK, STYLE_LINENUMBER, Theme::EditorBg);
    Call(SCI_STYLESETFORE, STYLE_CALLTIP, Theme::Bright);
    Call(SCI_STYLESETBACK, STYLE_CALLTIP, Hex(0x3C3F41));
    Call(SCI_STYLESETFORE, STYLE_BRACELIGHT, Theme::Bright);
    Call(SCI_STYLESETBACK, STYLE_BRACELIGHT, Hex(0x43454A));
    Call(SCI_CALLTIPSETBACK, Hex(0x3C3F41));
    Call(SCI_CALLTIPSETFORE, Theme::Bright);
    Call(SCI_CALLTIPSETFOREHLT, Theme::Link);  // syntax line of instruction hints

    Call(SCI_SETMARGINBACKN, 0, Theme::EditorBg);
    Call(SCI_SETMARGINBACKN, 2, Theme::EditorBg);
    Call(SCI_SETFOLDMARGINCOLOUR, 1, Theme::EditorBg);
    Call(SCI_SETFOLDMARGINHICOLOUR, 1, Theme::EditorBg);
    Call(SCI_SETELEMENTCOLOUR, SC_ELEMENT_CARET, Opaque(Hex(0xCED0D6)));
    // Translucent caret line drawn under the text, so marker backgrounds (errors, execution line) stay visible.
    Call(SCI_SETCARETLINELAYER, SC_LAYER_UNDER_TEXT);
    Call(SCI_SETELEMENTCOLOUR, SC_ELEMENT_CARET_LINE_BACK, Opaque(RGB(255, 255, 255), 0x0C));
    Call(SCI_SETELEMENTCOLOUR, SC_ELEMENT_SELECTION_BACK, Opaque(Theme::Selection));
    Call(SCI_SETELEMENTCOLOUR, SC_ELEMENT_SELECTION_INACTIVE_BACK, Opaque(Hex(0x2A3550)));
    Call(SCI_SETELEMENTCOLOUR, SC_ELEMENT_WHITE_SPACE, Opaque(Hex(0x4B5059)));

    int digitWidth = static_cast<int>(Call(SCI_TEXTWIDTH, STYLE_LINENUMBER, reinterpret_cast<sptr_t>("9")));
    Call(SCI_SETMARGINWIDTHN, 0, digitWidth * 2 + 6);
    Call(SCI_SETMARGINWIDTHN, 1, digitWidth * 5);
    Call(SCI_COLOURISE, 0, -1);
}

std::wstring Editor::Title() const {
    if (!file_.empty()) return file_.filename().wstring();
    if (!untitled_) const_cast<Editor*>(this)->untitled_ = g_nextUntitled++;
    return L"Безымянный-" + std::to_wstring(untitled_) + L".asm";
}

std::string Editor::Text() const {
    Sci_Position len = Call(SCI_GETLENGTH);
    const char* p = reinterpret_cast<const char*>(Call(SCI_GETCHARACTERPOINTER));
    return std::string(p, len);
}

std::wstring Editor::EolName() const {
    switch (Call(SCI_GETEOLMODE)) {
    case SC_EOL_LF: return L"LF";
    case SC_EOL_CR: return L"CR";
    default: return L"CRLF";
    }
}

int Editor::CaretLine() const {
    return static_cast<int>(Call(SCI_LINEFROMPOSITION, Call(SCI_GETCURRENTPOS))) + 1;
}

int Editor::CaretColumn() const {
    return static_cast<int>(Call(SCI_GETCOLUMN, Call(SCI_GETCURRENTPOS))) + 1;
}

bool Editor::Load(const fs::path& path, std::wstring& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = L"не удалось открыть файл";
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string data = ss.str();
    bom_ = data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xEF &&
           static_cast<unsigned char>(data[1]) == 0xBB && static_cast<unsigned char>(data[2]) == 0xBF;
    if (bom_) data.erase(0, 3);
    ansi_ = !data.empty() &&
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data.data(), (int)data.size(), nullptr, 0) == 0;
    if (ansi_) data = Narrow(Widen(data, 1251));

    int eol = SC_EOL_CRLF;
    size_t lf = data.find('\n');
    if (lf != std::string::npos) eol = (lf > 0 && data[lf - 1] == '\r') ? SC_EOL_CRLF : SC_EOL_LF;
    Call(SCI_SETEOLMODE, eol);
    SetText(data);
    Call(SCI_EMPTYUNDOBUFFER);
    Call(SCI_SETSAVEPOINT);
    file_ = NormalizePath(path);
    return true;
}

void Editor::SetText(const std::string& utf8) {
    Call(SCI_CLEARALL);
    Call(SCI_APPENDTEXT, utf8.size(), reinterpret_cast<sptr_t>(utf8.data()));
    Call(SCI_GOTOPOS, 0);
}

bool Editor::Save(const fs::path& path, std::wstring& error) {
    std::string data = Text();
    if (ansi_) data = Narrow(Widen(data), 1251);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = L"не удалось записать файл";
        return false;
    }
    if (bom_) out.write("\xEF\xBB\xBF", 3);
    out.write(data.data(), (std::streamsize)data.size());
    if (!out) {
        error = L"ошибка записи";
        return false;
    }
    file_ = NormalizePath(path);
    Call(SCI_SETSAVEPOINT);
    return true;
}

// ---- lexer ----------------------------------------------------------------------------------------------------

void Editor::Lex(Sci_Position endPos) {
    Sci_Position line = Call(SCI_LINEFROMPOSITION, Call(SCI_GETENDSTYLED));
    Sci_Position lastLine = Call(SCI_LINEFROMPOSITION, endPos);
    Sci_Position length = Call(SCI_GETLENGTH);
    for (; line <= lastLine; line++) {
        Sci_Position start = Call(SCI_POSITIONFROMLINE, line);
        Sci_Position end = Call(SCI_GETLINEENDPOSITION, line);
        Sci_Position next = line + 1 < Call(SCI_GETLINECOUNT) ? Call(SCI_POSITIONFROMLINE, line + 1) : length;
        if (start < 0 || next < start) break;
        const char* text = reinterpret_cast<const char*>(Call(SCI_GETRANGEPOINTER, start, end - start));
        Tokenize(text ? text : "", static_cast<int>(end - start), tokens_);
        Call(SCI_STARTSTYLING, start);
        Sci_Position cur = 0;
        for (const Token& t : tokens_) {
            if (t.start > cur) Call(SCI_SETSTYLING, t.start - cur, T_DEFAULT);
            Call(SCI_SETSTYLING, t.end - t.start, t.type);
            cur = t.end;
        }
        if (next - start > cur) Call(SCI_SETSTYLING, next - start - cur, T_DEFAULT);
    }
}

// ---- diagnostics ----------------------------------------------------------------------------------------------

void Editor::SetDiagnostics(const std::vector<Diagnostic>& list) {
    Sci_Position len = Call(SCI_GETLENGTH);
    for (int ind : {kIndicatorError, kIndicatorWarning}) {
        Call(SCI_SETINDICATORCURRENT, ind);
        Call(SCI_INDICATORCLEARRANGE, 0, len);
    }
    Call(SCI_MARKERDELETEALL, kMarkerError);
    Call(SCI_MARKERDELETEALL, kMarkerWarning);
    diagnostics_.clear();

    Sci_Position lines = Call(SCI_GETLINECOUNT);
    for (const Diagnostic& d : list) {
        if (d.line >= 1 && d.line <= lines) diagnostics_[d.line].push_back(d);
    }
    for (const auto& [line, diags] : diagnostics_) {
        bool error = std::any_of(diags.begin(), diags.end(), [](const Diagnostic& d) { return d.severity == Severity::Error; });
        Sci_Position l = line - 1;
        Call(SCI_MARKERADD, l, error ? kMarkerError : kMarkerWarning);
        Sci_Position start = Call(SCI_GETLINEINDENTPOSITION, l);
        Sci_Position end = Call(SCI_GETLINEENDPOSITION, l);
        std::string text = LineText(l);
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) {
            text.pop_back();
        }
        end = std::min(end, Call(SCI_POSITIONFROMLINE, l) + static_cast<Sci_Position>(text.size()));
        if (end > start) {
            Call(SCI_SETINDICATORCURRENT, error ? kIndicatorError : kIndicatorWarning);
            Call(SCI_INDICATORFILLRANGE, start, end - start);
        }
    }
}

const std::vector<Diagnostic>* Editor::DiagnosticsAt(int line) const {
    auto it = diagnostics_.find(line);
    return it == diagnostics_.end() ? nullptr : &it->second;
}

int Editor::Count(Severity s) const {
    int n = 0;
    for (const auto& [line, diags] : diagnostics_) {
        for (const Diagnostic& d : diags) n += d.severity == s;
    }
    return n;
}

void Editor::ShowDiagnosticTip(Sci_Position pos) {
    if (pos < 0) return;
    int line = static_cast<int>(Call(SCI_LINEFROMPOSITION, pos)) + 1;
    const auto* diags = DiagnosticsAt(line);
    if (!diags) return;
    std::wstring tip;
    for (const Diagnostic& d : *diags) {
        if (!tip.empty()) tip += L"\n";
        tip += (d.severity == Severity::Error ? L"Ошибка: " : L"Предупреждение: ") + d.message;
    }
    std::string utf8 = Narrow(tip);
    Call(SCI_CALLTIPSHOW, pos, reinterpret_cast<sptr_t>(utf8.c_str()));
}

// ---- editing helpers ------------------------------------------------------------------------------------------

std::string Editor::LineText(Sci_Position line) const {
    Sci_Position start = Call(SCI_POSITIONFROMLINE, line);
    Sci_Position end = Call(SCI_GETLINEENDPOSITION, line);
    if (end <= start) return {};
    const char* p = reinterpret_cast<const char*>(Call(SCI_GETRANGEPOINTER, start, end - start));
    return std::string(p, end - start);
}

void Editor::SelectedLines(Sci_Position& first, Sci_Position& last) const {
    Sci_Position s = Call(SCI_GETSELECTIONSTART);
    Sci_Position e = Call(SCI_GETSELECTIONEND);
    first = Call(SCI_LINEFROMPOSITION, s);
    last = Call(SCI_LINEFROMPOSITION, e);
    if (e > s && last > first && Call(SCI_POSITIONFROMLINE, last) == e) last--;
}

void Editor::OnCharAdded(int ch) {
    int eol = static_cast<int>(Call(SCI_GETEOLMODE));
    if (!((eol == SC_EOL_CR && ch == '\r') || (eol != SC_EOL_CR && ch == '\n'))) return;
    Sci_Position line = Call(SCI_LINEFROMPOSITION, Call(SCI_GETCURRENTPOS));
    if (line == 0) return;
    Sci_Position indent = Call(SCI_GETLINEINDENTATION, line - 1);
    std::string prev = LineText(line - 1);
    size_t comment = prev.find(';');
    if (comment != std::string::npos) prev.erase(comment);
    while (!prev.empty() && (prev.back() == ' ' || prev.back() == '\t')) prev.pop_back();
    if (indent == 0 && !prev.empty() && prev.back() == ':') indent = 4;  // body of a label
    if (indent > 0) {
        Call(SCI_SETLINEINDENTATION, line, indent);
        Call(SCI_GOTOPOS, Call(SCI_GETLINEINDENTPOSITION, line));
    }
}

void Editor::ToggleComment() {
    Sci_Position first, last;
    SelectedLines(first, last);
    bool allCommented = true;
    bool anyCode = false;
    for (Sci_Position l = first; l <= last; l++) {
        std::string t = LineText(l);
        size_t p = t.find_first_not_of(" \t");
        if (p == std::string::npos) continue;
        anyCode = true;
        if (t[p] != ';') allCommented = false;
    }
    if (!anyCode) allCommented = false;
    Call(SCI_BEGINUNDOACTION);
    for (Sci_Position l = last; l >= first; l--) {
        Sci_Position start = Call(SCI_POSITIONFROMLINE, l);
        std::string t = LineText(l);
        if (allCommented) {
            size_t idx = t.find(';');
            if (idx == std::string::npos) continue;
            Sci_Position n = (idx + 1 < t.size() && t[idx + 1] == ' ') ? 2 : 1;
            Call(SCI_DELETERANGE, start + idx, n);
        } else {
            if (t.find_first_not_of(" \t") == std::string::npos && first != last) continue;
            Call(SCI_INSERTTEXT, start, reinterpret_cast<sptr_t>("; "));
        }
    }
    Call(SCI_ENDUNDOACTION);
}

std::set<int> Editor::Breakpoints() const {
    std::set<int> lines;
    for (Sci_Position l = Call(SCI_MARKERNEXT, 0, 1 << kMarkerBreakpoint); l >= 0;
         l = Call(SCI_MARKERNEXT, l + 1, 1 << kMarkerBreakpoint)) {
        lines.insert(static_cast<int>(l) + 1);
    }
    return lines;
}

void Editor::SetBreakpoints(const std::set<int>& lines) {
    Call(SCI_MARKERDELETEALL, kMarkerBreakpoint);
    Sci_Position count = Call(SCI_GETLINECOUNT);
    for (int line : lines) {
        if (line >= 1 && line <= count) Call(SCI_MARKERADD, line - 1, kMarkerBreakpoint);
    }
}

void Editor::ToggleBreakpoint(int line) {
    Sci_Position l = line - 1;
    if (Call(SCI_MARKERGET, l) & (1 << kMarkerBreakpoint)) Call(SCI_MARKERDELETE, l, kMarkerBreakpoint);
    else Call(SCI_MARKERADD, l, kMarkerBreakpoint);
}

void Editor::SetExecutionLine(int line) {
    Call(SCI_MARKERDELETEALL, kMarkerExecArrow);
    Call(SCI_MARKERDELETEALL, kMarkerExecLine);
    if (line <= 0) return;
    Call(SCI_MARKERADD, line - 1, kMarkerExecArrow);
    Call(SCI_MARKERADD, line - 1, kMarkerExecLine);
}

bool Editor::WordRange(Sci_Position pos, Sci_Position& start, Sci_Position& end) const {
    Sci_Position len = Call(SCI_GETLENGTH);
    auto isWord = [](int c) { return isalnum(c) || c == '_' || c == '.' || c == '?' || c == '@' || c == '$' || c >= 0x80; };
    start = end = pos;
    while (start > 0 && isWord(static_cast<unsigned char>(Call(SCI_GETCHARAT, start - 1)))) start--;
    while (end < len && isWord(static_cast<unsigned char>(Call(SCI_GETCHARAT, end)))) end++;
    return end > start;
}

std::string Editor::WordAt(Sci_Position pos) const {
    Sci_Position s, e;
    if (!WordRange(pos, s, e)) return {};
    return std::string(reinterpret_cast<const char*>(Call(SCI_GETRANGEPOINTER, s, e - s)), e - s);
}

void Editor::GotoLine(int line) {
    Sci_Position l = std::clamp<Sci_Position>(line - 1, 0, Call(SCI_GETLINECOUNT) - 1);
    Call(SCI_ENSUREVISIBLEENFORCEPOLICY, l);
    Call(SCI_GOTOLINE, l);
    Sci_Position visible = Call(SCI_LINESONSCREEN);
    Call(SCI_SETFIRSTVISIBLELINE, std::max<Sci_Position>(0, l - visible / 2));
    SetFocus(hwnd_);
}
