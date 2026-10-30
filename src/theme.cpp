#include "common.h"

namespace Theme {

COLORREF EditorBg, PanelBg, InputBg, Border, Hover, Pressed, Text, Bright, Muted, Dim, Accent, Selection,
    InactiveSelection, CurrentLine, Error, Warning, Success, Link, CallTipBg, LineNumber, Changed;
COLORREF Comment, String, Number, Register, Instruction, Directive, Preprocessor, Label, Keyword, Macro, Function;
COLORREF ErrorLine, WarningLine, Breakpoint, ConditionalBreakpoint, ExecArrow, ExecLine, ListSelected, Occurrence;
COLORREF Ansi[16];
bool Dark = true;

namespace {

struct Palette {
    const wchar_t* id;
    const wchar_t* title;
    bool dark;
    // ui
    uint32_t editorBg, panelBg, inputBg, border, hover, pressed, text, bright, muted, dim, accent, selection,
        inactiveSelection, currentLine, error, warning, success, link, callTipBg, lineNumber, changed;
    // syntax
    uint32_t comment, string, number, reg, instruction, directive, preprocessor, label, keyword, macro, function;
    // markers
    uint32_t errorLine, warningLine, breakpoint, conditionalBreakpoint, execArrow, execLine, listSelected, occurrence;
    uint32_t ansi[16];
};

const Palette kPalettes[] = {
    {L"dark", L"Тёмная", true,
     0x1E1F22, 0x2B2D30, 0x1E1F22, 0x393B40, 0x3C3F45, 0x4A4D54, 0xBCBEC4, 0xDFE1E5, 0x868A91, 0x5F6369, 0x3574F0,
     0x214283, 0x2A3550, 0x26282E, 0xF75464, 0xE0B44A, 0x5FB865, 0x56A8F5, 0x3C3F41, 0x4B5059, 0xFF8F6B,
     0x7A7E85, 0x6AAB73, 0x2AACB8, 0xC77DBB, 0xCF8E6D, 0x56A8F5, 0xB3AE60, 0xFFC66D, 0xE0957B, 0x2AACB8, 0x6AAB73,
     0x3A2427, 0x37321F, 0xDB5C5C, 0xE0943A, 0xF2C55C, 0x3B3520, 0x2E436E, 0x3B4252,
     {0x7A7E85, 0xF75464, 0x5FB865, 0xE0B44A, 0x56A8F5, 0xC77DBB, 0x2AACB8, 0xBCBEC4, 0x9DA0A8, 0xFF8A95, 0x8BD48F,
      0xF2D27A, 0x8CC4FA, 0xE0A6D8, 0x6FD3DE, 0xFFFFFF}},
    {L"light", L"Светлая", false,
     0xFFFFFF, 0xF2F3F5, 0xFFFFFF, 0xD3D5DB, 0xE3E5E9, 0xD3D6DC, 0x2B2D30, 0x000000, 0x6C707E, 0x9A9EA8, 0x3574F0,
     0xA6D2FF, 0xD5E1F2, 0xF5F8FE, 0xD32F3F, 0x9E6C00, 0x1F8A3A, 0x2F6FD6, 0xF2F3F5, 0xA1A3AB, 0xC4501A,
     0x8C8C8C, 0x067D17, 0x1750EB, 0x871094, 0x0033B3, 0x7A3E9D, 0x9E880D, 0x00627A, 0xB34A00, 0x00796B, 0x067D17,
     0xFBE3E5, 0xFAF1D7, 0xE55765, 0xE0943A, 0xE0A800, 0xFAEFC4, 0xD5E1F2, 0xE4E8F4,
     {0x2B2D30, 0xC91B00, 0x168A16, 0x8A7000, 0x0225C7, 0xB01FAE, 0x00838F, 0x545454, 0x686868, 0xE52222, 0x10A010,
      0x9C8600, 0x1E5BE5, 0xC228C0, 0x00909E, 0x000000}},
    {L"monokai", L"Monokai", true,
     0x272822, 0x1E1F1C, 0x272822, 0x3E3D32, 0x3E3D32, 0x49483E, 0xF8F8F2, 0xFFFFFF, 0x90908A, 0x75715E, 0x4A7FD0,
     0x49483E, 0x3E3D32, 0x3E3D32, 0xF92672, 0xE6DB74, 0xA6E22E, 0x66D9EF, 0x3E3D32, 0x75715E, 0xFD971F,
     0x75715E, 0xE6DB74, 0xAE81FF, 0xFD971F, 0xF92672, 0x66D9EF, 0xCFCFC2, 0xA6E22E, 0x66D9EF, 0xAE81FF, 0xA6E22E,
     0x4A2530, 0x45422A, 0xF92672, 0xFD971F, 0xE6DB74, 0x4A4630, 0x49483E, 0x4A4A40,
     {0x75715E, 0xF92672, 0xA6E22E, 0xF4BF75, 0x66D9EF, 0xAE81FF, 0xA1EFE4, 0xF8F8F2, 0x90908A, 0xFF5C8F, 0xBEF05A,
      0xF8D58F, 0x8AE4F5, 0xC4A6FF, 0xBDF4EC, 0xFFFFFF}},
    {L"solarized-light", L"Solarized (светлая)", false,
     0xFDF6E3, 0xEEE8D5, 0xFDF6E3, 0xD9D2C2, 0xE4DDC8, 0xDCD4BC, 0x586E75, 0x073642, 0x839496, 0x93A1A1, 0x268BD2,
     0xCBE3F2, 0xE4E9EA, 0xEEE8D5, 0xDC322F, 0xB58900, 0x859900, 0x268BD2, 0xEEE8D5, 0x93A1A1, 0xCB4B16,
     0x93A1A1, 0x2AA198, 0xD33682, 0x6C71C4, 0x268BD2, 0x859900, 0xCB4B16, 0xB58900, 0x859900, 0xD33682, 0x2AA198,
     0xF7E0D5, 0xF3EBC8, 0xDC322F, 0xCB4B16, 0xB58900, 0xF1E7C0, 0xE4E9EA, 0xEAE3CB,
     {0x073642, 0xDC322F, 0x859900, 0xB58900, 0x268BD2, 0xD33682, 0x2AA198, 0x657B83, 0x586E75, 0xCB4B16, 0x859900,
      0xB58900, 0x268BD2, 0x6C71C4, 0x2AA198, 0x002B36}},
};

// Every theme defines the same slots; the default theme is applied before main() runs.
struct Init {
    Init() { Apply(L"dark"); }
} g_init;
}

const std::vector<Info>& All() {
    static std::vector<Info> list = [] {
        std::vector<Info> v;
        for (const Palette& p : kPalettes) v.push_back({p.id, p.title});
        return v;
    }();
    return list;
}

void Apply(const std::wstring& id) {
    const Palette* p = &kPalettes[0];
    for (const Palette& candidate : kPalettes) {
        if (id == candidate.id) p = &candidate;
    }
    Dark = p->dark;
    EditorBg = Hex(p->editorBg);
    PanelBg = Hex(p->panelBg);
    InputBg = Hex(p->inputBg);
    Border = Hex(p->border);
    Hover = Hex(p->hover);
    Pressed = Hex(p->pressed);
    Text = Hex(p->text);
    Bright = Hex(p->bright);
    Muted = Hex(p->muted);
    Dim = Hex(p->dim);
    Accent = Hex(p->accent);
    Selection = Hex(p->selection);
    InactiveSelection = Hex(p->inactiveSelection);
    CurrentLine = Hex(p->currentLine);
    Error = Hex(p->error);
    Warning = Hex(p->warning);
    Success = Hex(p->success);
    Link = Hex(p->link);
    CallTipBg = Hex(p->callTipBg);
    LineNumber = Hex(p->lineNumber);
    Changed = Hex(p->changed);
    Comment = Hex(p->comment);
    String = Hex(p->string);
    Number = Hex(p->number);
    Register = Hex(p->reg);
    Instruction = Hex(p->instruction);
    Directive = Hex(p->directive);
    Preprocessor = Hex(p->preprocessor);
    Label = Hex(p->label);
    Keyword = Hex(p->keyword);
    Macro = Hex(p->macro);
    Function = Hex(p->function);
    ErrorLine = Hex(p->errorLine);
    WarningLine = Hex(p->warningLine);
    Breakpoint = Hex(p->breakpoint);
    ConditionalBreakpoint = Hex(p->conditionalBreakpoint);
    ExecArrow = Hex(p->execArrow);
    ExecLine = Hex(p->execLine);
    ListSelected = Hex(p->listSelected);
    Occurrence = Hex(p->occurrence);
    for (int i = 0; i < 16; i++) Ansi[i] = Hex(p->ansi[i]);
}

COLORREF Blend(COLORREF a, COLORREF b, int percentOfB) {
    auto mix = [&](int x, int y) { return (x * (100 - percentOfB) + y * percentOfB) / 100; };
    return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)), mix(GetBValue(a), GetBValue(b)));
}

}
