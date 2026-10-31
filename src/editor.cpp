#include "editor.h"

#include "completion.h"
#include "tip.h"

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
    Call(SCI_SETMARGINS, 4);
    Call(SCI_SETMARGINTYPEN, 0, SC_MARGIN_SYMBOL);  // breakpoints and the execution arrow
    Call(SCI_SETMARGINMASKN, 0, (1 << kMarkerBreakpoint) | (1 << kMarkerConditional) | (1 << kMarkerLogpoint) | (1 << kMarkerExecArrow));
    Call(SCI_SETMARGINSENSITIVEN, 0, 1);
    Call(SCI_SETMARGINCURSORN, 0, SC_CURSORARROW);
    Call(SCI_SETMARGINTYPEN, 1, SC_MARGIN_NUMBER);
    Call(SCI_SETMARGINMASKN, 1, 0);
    Call(SCI_SETMARGINTYPEN, kGapMargin, SC_MARGIN_COLOUR);
    Call(SCI_SETMARGINMASKN, kGapMargin, 0);
    Call(SCI_SETMARGINWIDTHN, kGapMargin, 8);
    Call(SCI_SETCARETLINEVISIBLEALWAYS, 1);
    Call(SCI_SETCARETWIDTH, 2);
    Call(SCI_SETEXTRADESCENT, 2);
    Call(SCI_SETENDATLASTLINE, 0);
    Call(SCI_USEPOPUP, SC_POPUP_TEXT);
    Call(SCI_ASSIGNCMDKEY, 'D' + (SCMOD_CTRL << 16), SCI_SELECTIONDUPLICATE);

    Call(SCI_MARKERDEFINE, kMarkerError, SC_MARK_BACKGROUND);
    Call(SCI_MARKERDEFINE, kMarkerWarning, SC_MARK_BACKGROUND);
    Call(SCI_MARKERDEFINE, kMarkerBreakpoint, SC_MARK_CIRCLE);
    Call(SCI_MARKERDEFINE, kMarkerConditional, SC_MARK_CIRCLE);
    Call(SCI_MARKERDEFINE, kMarkerLogpoint, SC_MARK_SMALLRECT);
    Call(SCI_MARKERDEFINE, kMarkerExecArrow, SC_MARK_SHORTARROW);
    Call(SCI_MARKERDEFINE, kMarkerExecLine, SC_MARK_BACKGROUND);

    Call(SCI_INDICSETSTYLE, kIndicatorError, INDIC_SQUIGGLEPIXMAP);
    Call(SCI_INDICSETSTYLE, kIndicatorWarning, INDIC_SQUIGGLEPIXMAP);
    Call(SCI_INDICSETSTYLE, kIndicatorFind, INDIC_ROUNDBOX);
    Call(SCI_INDICSETALPHA, kIndicatorFind, 60);
    Call(SCI_INDICSETSTYLE, kIndicatorLink, INDIC_PLAIN);
    Call(SCI_INDICSETSTYLE, kIndicatorOccurrence, INDIC_FULLBOX);
    Call(SCI_INDICSETALPHA, kIndicatorOccurrence, 255);
    Call(SCI_INDICSETUNDER, kIndicatorOccurrence, 1);
    Call(SCI_INDICSETSTYLE, kIndicatorPair, INDIC_STRAIGHTBOX);
    Call(SCI_INDICSETALPHA, kIndicatorPair, 70);
    Call(SCI_INDICSETOUTLINEALPHA, kIndicatorPair, 160);
    Call(SCI_INDICSETUNDER, kIndicatorPair, 1);

    // Folding: %macro, %if, %rep blocks and the bodies of global labels (levels come from Lex).
    Call(SCI_SETMARGINTYPEN, kFoldMargin, SC_MARGIN_SYMBOL);
    Call(SCI_SETMARGINMASKN, kFoldMargin, SC_MASK_FOLDERS);
    Call(SCI_SETMARGINSENSITIVEN, kFoldMargin, 1);
    Call(SCI_SETAUTOMATICFOLD, SC_AUTOMATICFOLD_SHOW | SC_AUTOMATICFOLD_CLICK | SC_AUTOMATICFOLD_CHANGE);
    Call(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEROPEN, SC_MARK_BOXMINUS);
    Call(SCI_MARKERDEFINE, SC_MARKNUM_FOLDER, SC_MARK_BOXPLUS);
    Call(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERSUB, SC_MARK_VLINE);
    Call(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERTAIL, SC_MARK_LCORNER);
    Call(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEREND, SC_MARK_BOXPLUSCONNECTED);
    Call(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEROPENMID, SC_MARK_BOXMINUSCONNECTED);
    Call(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERMIDTAIL, SC_MARK_TCORNER);

    // Several carets: Alt+click, Alt+J (next occurrence), typing goes to every caret.
    Call(SCI_SETMULTIPLESELECTION, 1);
    Call(SCI_SETADDITIONALSELECTIONTYPING, 1);
    Call(SCI_SETMULTIPASTE, SC_MULTIPASTE_EACH);
    Call(SCI_SETVIRTUALSPACEOPTIONS, SCVS_RECTANGULARSELECTION);

    // Completion list: we filter and sort (case-insensitively) ourselves; "word?kind" picks the icon.
    Call(SCI_AUTOCSETTYPESEPARATOR, '?');
    Call(SCI_AUTOCSETIGNORECASE, 1);
    Call(SCI_AUTOCSETCASEINSENSITIVEBEHAVIOUR, SC_CASEINSENSITIVEBEHAVIOUR_IGNORECASE);
    Call(SCI_AUTOCSETORDER, SC_ORDER_PRESORTED);
    Call(SCI_AUTOCSETMAXHEIGHT, 12);
    Call(SCI_AUTOCSETMAXWIDTH, 40);
    Call(SCI_AUTOCSETDROPRESTOFWORD, 1);

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
    for (int kind = 1; kind <= static_cast<int>(Completion::Kind::Snippet); kind++) {
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
    StyleColors(*this, T_COMMENT, Theme::Comment, true);
    StyleColors(*this, T_STRING, Theme::String);
    StyleColors(*this, T_NUMBER, Theme::Number);
    StyleColors(*this, T_REGISTER, Theme::Register);
    StyleColors(*this, T_INSTRUCTION, Theme::Instruction);
    StyleColors(*this, T_DIRECTIVE, Theme::Directive);
    StyleColors(*this, T_PREPROCESSOR, Theme::Preprocessor);
    StyleColors(*this, T_LABEL, Theme::Label);
    StyleColors(*this, T_KEYWORD, Theme::Keyword);

    Call(SCI_STYLESETFORE, STYLE_LINENUMBER, Theme::LineNumber);
    Call(SCI_STYLESETBACK, STYLE_LINENUMBER, Theme::EditorBg);
    Call(SCI_STYLESETFORE, STYLE_CALLTIP, Theme::Bright);
    Call(SCI_STYLESETBACK, STYLE_CALLTIP, Theme::CallTipBg);
    Call(SCI_STYLESETFORE, STYLE_BRACELIGHT, Theme::Bright);
    Call(SCI_STYLESETBACK, STYLE_BRACELIGHT, Theme::Blend(Theme::EditorBg, Theme::Accent, 30));
    Call(SCI_STYLESETBOLD, STYLE_BRACELIGHT, 1);
    Call(SCI_STYLESETFORE, STYLE_BRACEBAD, Theme::Error);
    Call(SCI_STYLESETFORE, kStyleNote, Theme::Muted);
    Call(SCI_STYLESETBACK, kStyleNote, Theme::Blend(Theme::EditorBg, Theme::Muted, 10));
    Call(SCI_STYLESETITALIC, kStyleNote, 1);
    Call(SCI_STYLESETSIZE, kStyleNote, std::max(6, fontSize - 1));
    Call(SCI_CALLTIPSETBACK, Theme::CallTipBg);
    Call(SCI_CALLTIPSETFORE, Theme::Bright);
    Call(SCI_CALLTIPSETFOREHLT, Theme::Link);  // syntax line of instruction hints

    Call(SCI_MARKERSETBACK, kMarkerError, Theme::ErrorLine);
    Call(SCI_MARKERSETBACK, kMarkerWarning, Theme::WarningLine);
    Call(SCI_MARKERSETFORE, kMarkerBreakpoint, Theme::Breakpoint);
    Call(SCI_MARKERSETBACK, kMarkerBreakpoint, Theme::Breakpoint);
    Call(SCI_MARKERSETFORE, kMarkerConditional, Theme::ConditionalBreakpoint);
    Call(SCI_MARKERSETBACK, kMarkerConditional, Theme::ConditionalBreakpoint);
    Call(SCI_MARKERSETFORE, kMarkerLogpoint, Theme::ConditionalBreakpoint);
    Call(SCI_MARKERSETBACK, kMarkerLogpoint, Theme::ConditionalBreakpoint);
    Call(SCI_MARKERSETFORE, kMarkerExecArrow, Theme::EditorBg);
    Call(SCI_MARKERSETBACK, kMarkerExecArrow, Theme::ExecArrow);
    Call(SCI_MARKERSETBACK, kMarkerExecLine, Theme::ExecLine);
    for (int m = SC_MARKNUM_FOLDEREND; m <= SC_MARKNUM_FOLDEROPEN; m++) {
        Call(SCI_MARKERSETFORE, m, Theme::EditorBg);
        Call(SCI_MARKERSETBACK, m, Theme::LineNumber);
        Call(SCI_MARKERSETBACKSELECTED, m, Theme::Muted);
    }
    Call(SCI_INDICSETFORE, kIndicatorError, Theme::Error);
    Call(SCI_INDICSETFORE, kIndicatorWarning, Theme::Warning);
    Call(SCI_INDICSETFORE, kIndicatorFind, Theme::Accent);
    Call(SCI_INDICSETFORE, kIndicatorLink, Theme::Link);
    Call(SCI_INDICSETFORE, kIndicatorOccurrence, Theme::Occurrence);
    Call(SCI_INDICSETFORE, kIndicatorPair, Theme::Accent);

    Call(SCI_SETELEMENTCOLOUR, SC_ELEMENT_LIST, Opaque(Theme::Text));
    Call(SCI_SETELEMENTCOLOUR, SC_ELEMENT_LIST_BACK, Opaque(Theme::PanelBg));
    Call(SCI_SETELEMENTCOLOUR, SC_ELEMENT_LIST_SELECTED, Opaque(Theme::Bright));
    Call(SCI_SETELEMENTCOLOUR, SC_ELEMENT_LIST_SELECTED_BACK, Opaque(Theme::ListSelected));
    RegisterCompletionIcons();

    Call(SCI_SETMARGINBACKN, 0, Theme::EditorBg);
    Call(SCI_SETMARGINBACKN, kGapMargin, Theme::EditorBg);
    Call(SCI_SETFOLDMARGINCOLOUR, 1, Theme::EditorBg);
    Call(SCI_SETFOLDMARGINHICOLOUR, 1, Theme::EditorBg);
    Call(SCI_SETELEMENTCOLOUR, SC_ELEMENT_CARET, Opaque(Theme::Bright));
    // Translucent caret line drawn under the text, so marker backgrounds (errors, execution line) stay visible.
    Call(SCI_SETCARETLINELAYER, SC_LAYER_UNDER_TEXT);
    Call(SCI_SETELEMENTCOLOUR, SC_ELEMENT_CARET_LINE_BACK,
         Theme::Dark ? Opaque(RGB(255, 255, 255), 0x0C) : Opaque(RGB(0, 0, 0), 0x08));
    Call(SCI_SETELEMENTCOLOUR, SC_ELEMENT_SELECTION_BACK, Opaque(Theme::Selection));
    Call(SCI_SETELEMENTCOLOUR, SC_ELEMENT_SELECTION_ADDITIONAL_BACK, Opaque(Theme::Selection));
    Call(SCI_SETELEMENTCOLOUR, SC_ELEMENT_SELECTION_INACTIVE_BACK, Opaque(Theme::InactiveSelection));
    Call(SCI_SETELEMENTCOLOUR, SC_ELEMENT_CARET_ADDITIONAL, Opaque(Theme::Bright));
    Call(SCI_SETELEMENTCOLOUR, SC_ELEMENT_WHITE_SPACE, Opaque(Theme::LineNumber));

    int digitWidth = static_cast<int>(Call(SCI_TEXTWIDTH, STYLE_LINENUMBER, reinterpret_cast<sptr_t>("9")));
    Call(SCI_SETMARGINWIDTHN, 0, digitWidth * 2 + 6);
    Call(SCI_SETMARGINWIDTHN, 1, digitWidth * 5);
    Call(SCI_SETMARGINWIDTHN, kFoldMargin, digitWidth + 6);
    Call(SCI_SETMARGINBACKN, kFoldMargin, Theme::EditorBg);
    Call(SCI_COLOURISE, 0, -1);
}

std::wstring Editor::Title() const {
    if (!file_.empty()) return file_.filename().wstring();
    if (!untitled_) const_cast<Editor*>(this)->untitled_ = g_nextUntitled++;
    return Tr(L"Безымянный-") + std::to_wstring(untitled_) + L".asm";
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
        error = Tr(L"не удалось открыть файл");
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
    RememberDiskTime();
    return true;
}

void Editor::RememberDiskTime() {
    std::error_code ec;
    diskTime_ = fs::last_write_time(file_, ec);
    if (ec) diskTime_ = {};
}

bool Editor::ChangedOnDisk() const {
    if (file_.empty()) return false;
    std::error_code ec;
    auto t = fs::last_write_time(file_, ec);
    return !ec && t != diskTime_;
}

void Editor::ForgetDiskChange() {
    RememberDiskTime();
}

bool Editor::Reload(std::wstring& error) {
    Sci_Position caret = Call(SCI_GETCURRENTPOS);
    Sci_Position first = Call(SCI_GETFIRSTVISIBLELINE);
    std::map<int, std::string> bps = Breakpoints();
    if (!Load(file_, error)) return false;
    SetBreakpoints(bps);
    Call(SCI_GOTOPOS, std::min(caret, Call(SCI_GETLENGTH)));
    Call(SCI_SETFIRSTVISIBLELINE, first);
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
        error = Tr(L"не удалось записать файл");
        return false;
    }
    if (bom_) out.write("\xEF\xBB\xBF", 3);
    out.write(data.data(), (std::streamsize)data.size());
    if (!out) {
        error = Tr(L"ошибка записи");
        return false;
    }
    out.close();
    file_ = NormalizePath(path);
    Call(SCI_SETSAVEPOINT);
    RememberDiskTime();
    return true;
}

// ---- lexer ----------------------------------------------------------------------------------------------------

// Styles lines with the tokenizer and computes fold levels. The line state keeps what the fold level of the next
// line depends on: the nesting of %macro/%if/%rep blocks (bits 0-7) and whether we are in a global label's body
// (bit 8). When a line's state changes, the following lines are redone until the state matches again.
void Editor::Lex(Sci_Position endPos) {
    Sci_Position line = Call(SCI_LINEFROMPOSITION, Call(SCI_GETENDSTYLED));
    Sci_Position lastLine = Call(SCI_LINEFROMPOSITION, endPos);
    Sci_Position length = Call(SCI_GETLENGTH);
    Sci_Position lineCount = Call(SCI_GETLINECOUNT);
    auto word = [&](const char* text, const Token& t) {
        std::string w(text + t.start, t.end - t.start);
        for (char& c : w) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        return w;
    };
    for (; line < lineCount; line++) {
        Sci_Position start = Call(SCI_POSITIONFROMLINE, line);
        Sci_Position end = Call(SCI_GETLINEENDPOSITION, line);
        Sci_Position next = line + 1 < lineCount ? Call(SCI_POSITIONFROMLINE, line + 1) : length;
        if (start < 0 || next < start) break;
        const char* text = reinterpret_cast<const char*>(Call(SCI_GETRANGEPOINTER, start, end - start));
        if (!text) text = "";
        Tokenize(text, static_cast<int>(end - start), tokens_);
        Call(SCI_STARTSTYLING, start);
        Sci_Position cur = 0;
        for (const Token& t : tokens_) {
            if (t.start > cur) Call(SCI_SETSTYLING, t.start - cur, T_DEFAULT);
            Call(SCI_SETSTYLING, t.end - t.start, t.type);
            cur = t.end;
        }
        if (next - start > cur) Call(SCI_SETSTYLING, next - start - cur, T_DEFAULT);

        int before = line > 0 ? static_cast<int>(Call(SCI_GETLINESTATE, line - 1)) : 0;
        int depth = before & 0xFF, body = (before >> 8) & 1;
        int level = SC_FOLDLEVELBASE + body + depth;
        if (!tokens_.empty()) {
            const Token& t = tokens_[0];
            std::string w = word(text, t);
            if (t.type == T_LABEL && w.find(':') != std::string::npos && w[0] != '.') {
                level = SC_FOLDLEVELBASE + depth;  // a global label starts a function body
                body = 1;
            } else if (t.type == T_DIRECTIVE && (w == "section" || w == "segment")) {
                level = SC_FOLDLEVELBASE + depth;
                body = 0;
            } else if (t.type == T_PREPROCESSOR) {
                if (w == "%macro" || w == "%imacro" || w == "%rep" || w.rfind("%if", 0) == 0) {
                    depth = std::min(depth + 1, 0xFF);
                } else if ((w == "%endmacro" || w == "%endrep" || w == "%endif") && depth > 0) {
                    depth--;
                }
            }
        }
        int after = depth | (body << 8);
        int nextLevel = SC_FOLDLEVELBASE + body + depth;
        Call(SCI_SETFOLDLEVEL, line, level | (nextLevel > level ? SC_FOLDLEVELHEADERFLAG : 0));
        int old = static_cast<int>(Call(SCI_GETLINESTATE, line));
        Call(SCI_SETLINESTATE, line, after);
        if (line >= lastLine && old == after) break;
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
        tip += std::wstring(d.severity == Severity::Error ? Tr(L"Ошибка: ") : Tr(L"Предупреждение: ")) + d.message;
    }
    ShowTip(pos, Narrow(tip));
}

Sci_Position Editor::ScrollLines() const {
    Sci_Position last = Call(SCI_GETLINECOUNT) - 1;
    return Call(SCI_VISIBLEFROMDOCLINE, last) + Call(SCI_WRAPCOUNT, last) + kScrollPastEnd;
}

void Editor::LimitScroll() const {
    Sci_Position maxTop = std::max<Sci_Position>(0, ScrollLines() - Call(SCI_LINESONSCREEN));
    if (Call(SCI_GETFIRSTVISIBLELINE) > maxTop) Call(SCI_SETFIRSTVISIBLELINE, maxTop);
}

void Editor::ShowTip(Sci_Position pos, const std::string& utf8, size_t hltFrom, size_t hltTo) const {
    POINT p{static_cast<LONG>(Call(SCI_POINTXFROMPOSITION, 0, pos)), static_cast<LONG>(Call(SCI_POINTYFROMPOSITION, 0, pos))};
    int height = static_cast<int>(Call(SCI_TEXTHEIGHT, Call(SCI_LINEFROMPOSITION, pos)));
    ClientToScreen(hwnd_, &p);
    Tip::Show(hwnd_, p.x, p.y, p.y + height, utf8, hltFrom, hltTo);
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

std::map<int, std::string> Editor::Breakpoints() const {
    std::map<int, std::string> lines;
    int mask = (1 << kMarkerBreakpoint) | (1 << kMarkerConditional) | (1 << kMarkerLogpoint);
    for (Sci_Position l = Call(SCI_MARKERNEXT, 0, mask); l >= 0; l = Call(SCI_MARKERNEXT, l + 1, mask)) {
        std::string condition;
        for (int i = 0;; i++) {
            int handle = static_cast<int>(Call(SCI_MARKERHANDLEFROMLINE, l, i));
            if (handle < 0) break;
            auto it = conditions_.find(handle);
            if (it != conditions_.end()) condition = it->second;
        }
        lines[static_cast<int>(l) + 1] = condition;
    }
    return lines;
}

void Editor::SetBreakpoints(const std::map<int, std::string>& lines) {
    Call(SCI_MARKERDELETEALL, kMarkerBreakpoint);
    Call(SCI_MARKERDELETEALL, kMarkerConditional);
    Call(SCI_MARKERDELETEALL, kMarkerLogpoint);
    conditions_.clear();
    Sci_Position count = Call(SCI_GETLINECOUNT);
    for (const auto& [line, condition] : lines) {
        if (line < 1 || line > count) continue;
        if (condition.empty()) {
            Call(SCI_MARKERADD, line - 1, kMarkerBreakpoint);
        } else {
            int marker = IsLogpoint(condition) ? kMarkerLogpoint : kMarkerConditional;
            int handle = static_cast<int>(Call(SCI_MARKERADD, line - 1, marker));
            conditions_[handle] = condition;
        }
    }
}

bool Editor::HasBreakpoint(int line) const {
    return (Call(SCI_MARKERGET, line - 1) &
            ((1 << kMarkerBreakpoint) | (1 << kMarkerConditional) | (1 << kMarkerLogpoint))) != 0;
}

void Editor::ToggleBreakpoint(int line) {
    if (!HasBreakpoint(line)) {
        Call(SCI_MARKERADD, line - 1, kMarkerBreakpoint);
        return;
    }
    SetBreakpointCondition(line, {});  // drops the condition, leaves an ordinary marker
    Call(SCI_MARKERDELETE, line - 1, kMarkerBreakpoint);
}

// Replaces the breakpoint on a line by one with the condition (an empty condition makes an ordinary one).
void Editor::SetBreakpointCondition(int line, const std::string& condition) {
    Sci_Position l = line - 1;
    for (int i = 0;; i++) {
        int handle = static_cast<int>(Call(SCI_MARKERHANDLEFROMLINE, l, i));
        if (handle < 0) break;
        conditions_.erase(handle);
    }
    Call(SCI_MARKERDELETE, l, kMarkerConditional);
    Call(SCI_MARKERDELETE, l, kMarkerLogpoint);
    Call(SCI_MARKERDELETE, l, kMarkerBreakpoint);
    if (condition.empty()) {
        Call(SCI_MARKERADD, l, kMarkerBreakpoint);
    } else {
        int handle = static_cast<int>(Call(SCI_MARKERADD, l, IsLogpoint(condition) ? kMarkerLogpoint : kMarkerConditional));
        conditions_[handle] = condition;
    }
}

void Editor::SetLineNotes(const std::map<int, std::string>& notes) {
    Call(SCI_EOLANNOTATIONCLEARALL);
    Call(SCI_EOLANNOTATIONSETVISIBLE, notes.empty() ? EOLANNOTATION_HIDDEN : EOLANNOTATION_BOXED);
    Sci_Position count = Call(SCI_GETLINECOUNT);
    for (const auto& [line, text] : notes) {
        if (line < 1 || line > count) continue;
        Call(SCI_EOLANNOTATIONSETTEXT, line - 1, reinterpret_cast<sptr_t>(text.c_str()));
        Call(SCI_EOLANNOTATIONSETSTYLE, line - 1, kStyleNote);
    }
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
