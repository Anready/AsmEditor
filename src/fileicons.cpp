#include "fileicons.h"

namespace FileIcons {
namespace {

constexpr int kSamples = 4;                    // drawn this many times larger, then averaged: smooth edges
constexpr COLORREF kKey = RGB(255, 0, 255);    // the background while drawing (no icon uses it)

// Drawing in a 16 x 16 grid of design units, scaled to the canvas; GDI without anti-aliasing (the averaging does it).
struct Canvas {
    HDC dc;
    double k;
    int X(double v) const { return static_cast<int>(v * k + 0.5); }
    void Brush(COLORREF c) const {
        SetDCBrushColor(dc, c);
        SelectObject(dc, GetStockObject(DC_BRUSH));
        SelectObject(dc, GetStockObject(NULL_PEN));
    }
    void Rect(double l, double t, double r, double b, COLORREF c) const {
        RECT rc{X(l), X(t), X(r), X(b)};
        SetDCBrushColor(dc, c);
        FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }
    void Round(double l, double t, double r, double b, double radius, COLORREF c) const {
        Brush(c);
        RoundRect(dc, X(l), X(t), X(r) + 1, X(b) + 1, X(2 * radius), X(2 * radius));
    }
    void Oval(double l, double t, double r, double b, COLORREF c) const {
        Brush(c);
        Ellipse(dc, X(l), X(t), X(r) + 1, X(b) + 1);
    }
    void Poly(std::initializer_list<std::pair<double, double>> points, COLORREF c) const {
        std::vector<POINT> p;
        for (auto [x, y] : points) p.push_back({X(x), X(y)});
        Brush(c);
        Polygon(dc, p.data(), static_cast<int>(p.size()));
    }
    void Text(const wchar_t* s, double l, double t, double r, double b, double height, COLORREF c) const {
        HFONT font = CreateFontW(-X(height), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ old = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, c);
        RECT rc{X(l), X(t), X(r), X(b)};
        DrawTextW(dc, s, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, old);
        DeleteObject(font);
    }
    // A rounded square with a label: most languages, as in IntelliJ.
    void Badge(COLORREF fill, const wchar_t* label, double height, COLORREF text = RGB(255, 255, 255)) const {
        Round(1.5, 1.5, 14.5, 14.5, 3, fill);
        if (*label) Text(label, 1.5, 1, 14.5, 14, height, text);
    }
    void Page(COLORREF fill, COLORREF corner) const {
        Poly({{3, 1}, {10, 1}, {13, 4}, {13, 15}, {3, 15}}, fill);
        Poly({{10, 1}, {10, 4}, {13, 4}}, corner);
    }
    // A chip: the assembly files.
    void Chip(COLORREF body, COLORREF pins) const {
        for (double p : {4.6, 7.35, 10.1}) {
            Rect(p, 1, p + 1.3, 3.5, pins);
            Rect(p, 12.5, p + 1.3, 15, pins);
            Rect(1, p, 3.5, p + 1.3, pins);
            Rect(12.5, p, 15, p + 1.3, pins);
        }
        Round(3, 3, 13, 13, 1.6, body);
        Oval(4.6, 4.6, 6.6, 6.6, Theme::Blend(body, RGB(255, 255, 255), 55));
    }
};

void Paint(const Canvas& c, Kind kind) {
    const COLORREF white = RGB(255, 255, 255);
    switch (kind) {
    case Kind::Folder:
        c.Round(1, 2.5, 7.5, 6.5, 1, Hex(0x6E85A3));
        c.Round(1, 4.2, 15, 13.5, 1.5, Hex(0x8BA2C0));
        break;
    case Kind::FolderOpen:
        c.Round(1, 2.5, 7.5, 6.5, 1, Hex(0x6E85A3));
        c.Round(1, 4.2, 14, 13.5, 1.5, Hex(0x6E85A3));
        c.Poly({{3.2, 7.2}, {15.6, 7.2}, {13.4, 13.5}, {1, 13.5}}, Hex(0x9DB3CF));
        break;
    case Kind::Asm:
        c.Chip(Hex(0x9B7CF0), Hex(0x7D66C2));
        break;
    case Kind::Include:
        c.Chip(Hex(0x3FAF95), Hex(0x338E79));
        break;
    case Kind::C:
        c.Badge(Hex(0x4A86E8), L"C", 10);
        break;
    case Kind::Cpp:
        c.Round(1.5, 1.5, 14.5, 14.5, 3, Hex(0x3D63D1));
        c.Text(L"C", 1.5, 1, 11, 14, 10, white);
        c.Text(L"+", 8.5, 1.5, 15, 9, 7, white);
        break;
    case Kind::Header:
        c.Badge(Hex(0xA06CD5), L"H", 10);
        break;
    case Kind::Python:
        c.Round(1, 1, 10.5, 10.5, 2.6, Hex(0x3D7AB8));
        c.Round(5.5, 5.5, 15, 15, 2.6, Hex(0xF2C744));
        c.Oval(2.8, 2.8, 4.8, 4.8, white);
        c.Oval(11.2, 11.2, 13.2, 13.2, white);
        break;
    case Kind::Text:
        c.Page(Hex(0xAEB5BE), Hex(0x7E868F));
        for (double y : {6.5, 9.0, 11.5}) c.Rect(5, y, 11, y + 1.1, Hex(0x5F666E));
        break;
    case Kind::Markdown:
        c.Badge(Hex(0x5B8DBD), L"M", 9.5);
        break;
    case Kind::Config:
        c.Badge(Hex(0xDE953A), L"{}", 8.5);
        break;
    case Kind::Script:
        c.Badge(Hex(0x4B5563), L">_", 7.5, Hex(0x9BE39B));
        break;
    case Kind::Exe:
        c.Round(1, 2, 15, 14, 2, Hex(0x4A90D9));
        c.Round(2.4, 5, 13.6, 12.6, 1, Hex(0xDCE9F7));
        break;
    case Kind::Binary:
        c.Badge(Hex(0x7C8591), L"01", 7.5);
        break;
    case Kind::Archive:
        c.Badge(Hex(0xB5813F), L"", 0);
        for (double y = 2; y < 10.5; y += 2) {
            c.Rect(7, y, 8, y + 1, Hex(0xF1DDBF));
            c.Rect(8, y + 1, 9, y + 2, Hex(0xF1DDBF));
        }
        c.Round(6.2, 10.5, 9.8, 13.5, 0.8, Hex(0xF1DDBF));
        break;
    case Kind::Image:
        c.Round(1, 2, 15, 14, 2, Hex(0x45A57A));
        c.Poly({{2.5, 12.6}, {6.5, 7}, {9, 10.2}, {10.6, 8.6}, {13.5, 12.6}}, Hex(0xE6F5EC));
        c.Oval(10, 4, 12.6, 6.6, Hex(0xE6F5EC));
        break;
    case Kind::Web:
        c.Badge(Hex(0xE2673B), L"<>", 8);
        break;
    case Kind::JavaScript:
        c.Badge(Hex(0xE8C547), L"JS", 7.5, Hex(0x2B2B2B));
        break;
    case Kind::Function:
        c.Oval(1.5, 1.5, 14.5, 14.5, Hex(0x4D8FE0));
        c.Text(L"f", 1.5, 1, 14.5, 14, 10, white);
        break;
    case Kind::Macro:
        c.Oval(1.5, 1.5, 14.5, 14.5, Hex(0x9E78D8));
        c.Text(L"m", 1.5, 0.5, 14.5, 14, 10, white);
        break;
    case Kind::Section:
        c.Badge(Hex(0x4C9A8E), L"S", 9.5);
        break;
    case Kind::Command:
        c.Badge(Hex(0x5A6470), L"›", 11);
        break;
    default:
        c.Page(Hex(0xAEB5BE), Hex(0x7E868F));
        break;
    }
}

HBITMAP MakeDib(int w, int h, uint32_t** bits) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(bits), nullptr, 0);
}

// The icon as a 32-bit bitmap with premultiplied alpha (for AlphaBlend); straight alpha for icons.
HBITMAP Render(Kind kind, int size, bool premultiplied) {
    int n = size * kSamples;
    uint32_t* big = nullptr;
    HBITMAP canvas = MakeDib(n, n, &big);
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(dc, canvas);
    RECT all{0, 0, n, n};
    SetDCBrushColor(dc, kKey);
    FillRect(dc, &all, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    Paint(Canvas{dc, n / 16.0}, kind);
    GdiFlush();

    uint32_t* out = nullptr;
    HBITMAP result = MakeDib(size, size, &out);
    const uint32_t key = (GetRValue(kKey) << 16) | (GetGValue(kKey) << 8) | GetBValue(kKey);
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            int count = 0, r = 0, g = 0, b = 0;
            for (int sy = 0; sy < kSamples; sy++) {
                for (int sx = 0; sx < kSamples; sx++) {
                    uint32_t p = big[(y * kSamples + sy) * n + x * kSamples + sx] & 0xFFFFFF;
                    if (p == key) continue;
                    count++;
                    r += (p >> 16) & 0xFF;
                    g += (p >> 8) & 0xFF;
                    b += p & 0xFF;
                }
            }
            const int area = kSamples * kSamples;
            uint32_t a = static_cast<uint32_t>(count * 255 / area);
            int div = premultiplied ? area : std::max(1, count);
            out[y * size + x] = count ? (a << 24) | ((r / div) << 16) | ((g / div) << 8) | (b / div) : 0;
        }
    }
    SelectObject(dc, old);
    DeleteDC(dc);
    DeleteObject(canvas);
    return result;
}

HBITMAP Cached(Kind kind, int size) {
    static std::map<std::pair<int, int>, HBITMAP> cache;
    HBITMAP& bmp = cache[{static_cast<int>(kind), size}];
    if (!bmp) bmp = Render(kind, size, true);
    return bmp;
}
}

Kind KindOf(const fs::path& path, bool folder, bool open) {
    if (folder) return open ? Kind::FolderOpen : Kind::Folder;
    std::wstring ext = Lower(path.extension().wstring());
    if (!ext.empty()) ext.erase(0, 1);
    static const std::map<std::wstring, Kind> kinds = {
        {L"asm", Kind::Asm},        {L"nasm", Kind::Asm},       {L"s", Kind::Asm},          {L"yasm", Kind::Asm},
        {L"inc", Kind::Include},    {L"mac", Kind::Include},    {L"c", Kind::C},            {L"cpp", Kind::Cpp},
        {L"cc", Kind::Cpp},         {L"cxx", Kind::Cpp},        {L"h", Kind::Header},       {L"hpp", Kind::Header},
        {L"hh", Kind::Header},      {L"hxx", Kind::Header},     {L"py", Kind::Python},      {L"pyw", Kind::Python},
        {L"txt", Kind::Text},       {L"log", Kind::Text},       {L"lst", Kind::Text},       {L"map", Kind::Text},
        {L"md", Kind::Markdown},    {L"json", Kind::Config},    {L"ini", Kind::Config},     {L"cfg", Kind::Config},
        {L"toml", Kind::Config},    {L"yaml", Kind::Config},    {L"yml", Kind::Config},     {L"conf", Kind::Config},
        {L"bat", Kind::Script},     {L"cmd", Kind::Script},     {L"ps1", Kind::Script},     {L"sh", Kind::Script},
        {L"exe", Kind::Exe},        {L"dll", Kind::Exe},        {L"com", Kind::Exe},        {L"obj", Kind::Binary},
        {L"o", Kind::Binary},       {L"lib", Kind::Binary},     {L"a", Kind::Binary},       {L"bin", Kind::Binary},
        {L"pdb", Kind::Binary},     {L"ilk", Kind::Binary},     {L"res", Kind::Binary},     {L"exp", Kind::Binary},
        {L"zip", Kind::Archive},    {L"7z", Kind::Archive},     {L"rar", Kind::Archive},    {L"gz", Kind::Archive},
        {L"png", Kind::Image},      {L"jpg", Kind::Image},      {L"jpeg", Kind::Image},     {L"bmp", Kind::Image},
        {L"gif", Kind::Image},      {L"ico", Kind::Image},      {L"svg", Kind::Image},      {L"html", Kind::Web},
        {L"htm", Kind::Web},        {L"xml", Kind::Web},        {L"css", Kind::Web},        {L"js", Kind::JavaScript},
        {L"ts", Kind::JavaScript},
    };
    auto it = kinds.find(ext);
    return it == kinds.end() ? Kind::File : it->second;
}

void Draw(HDC dc, int x, int y, int size, Kind kind) {
    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mem, Cached(kind, size));
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    AlphaBlend(dc, x, y, size, size, mem, 0, 0, size, size, blend);
    SelectObject(mem, old);
    DeleteDC(mem);
}

HIMAGELIST ImageList(int size) {
    static std::map<int, HIMAGELIST> lists;
    HIMAGELIST& list = lists[size];
    if (list) return list;
    int count = static_cast<int>(Kind::Count);
    list = ImageList_Create(size, size, ILC_COLOR32, count, 0);
    // Icons have straight alpha and a mask; the image list then draws them blended.
    std::vector<BYTE> zeros(static_cast<size_t>((size + 15) / 16 * 2) * size, 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, zeros.data());
    for (int i = 0; i < count; i++) {
        HBITMAP color = Render(static_cast<Kind>(i), size, false);
        ICONINFO info{TRUE, 0, 0, mask, color};
        HICON icon = CreateIconIndirect(&info);
        ImageList_AddIcon(list, icon);
        DestroyIcon(icon);
        DeleteObject(color);
    }
    DeleteObject(mask);
    return list;
}
}
