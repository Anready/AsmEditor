#include "changemarks.h"

#include "editor.h"

#include <cmath>

namespace {

// Image markers: for added and changed lines a single line, the top, the middle and the bottom of a run; one for a
// deleted place.
constexpr int kFirstShape = 14;
enum Shape { Single, Top, Middle, Bottom };
int MarkerOf(LineDiff::Change change, Shape shape) {
    if (change == LineDiff::Change::Deleted) return kFirstShape + 8;
    return kFirstShape + (change == LineDiff::Change::Modified ? 4 : 0) + shape;
}

std::map<HWND, std::string> g_defined;  // editor -> line height, width and colors its images were made for

// A bar the width of the margin, rounded (radius = half its width) where the run starts / ends; smooth edges.
std::vector<uint8_t> Image(int w, int h, COLORREF color, bool roundTop, bool roundBottom, int length) {
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4, 0);
    double r = w / 2.0;
    double from = roundTop ? r : -1e9, to = roundBottom ? length - r : 1e9;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            double cx = x + 0.5, cy = y + 0.5;
            double coverage = 0;
            if (cy <= length || !roundBottom) {
                double d = std::hypot(cx - r, cy - std::clamp(cy, from, to));
                coverage = std::clamp(r - d + 0.5, 0.0, 1.0);
            }
            uint8_t* p = &rgba[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = GetRValue(color);
            p[1] = GetGValue(color);
            p[2] = GetBValue(color);
            p[3] = static_cast<uint8_t>(coverage * 255 + 0.5);
        }
    }
    return rgba;
}

// (Re)defines the images when the line height, the margin width or the theme changed.
void Define(Editor& ed) {
    int w = static_cast<int>(ed.Call(SCI_GETMARGINWIDTHN, Editor::kChangeMargin)) - 1;
    int h = static_cast<int>(ed.Call(SCI_TEXTHEIGHT, 0));
    if (w < 2 || h < 2) return;
    COLORREF added = Theme::Blend(Theme::EditorBg, Theme::Success, 75), modified = Theme::Blend(Theme::EditorBg, Theme::Link, 75),
             deleted = Theme::Blend(Theme::EditorBg, Theme::Muted, 75);
    std::string key = std::to_string(w) + "/" + std::to_string(h) + "/" + std::to_string(added) + "/" + std::to_string(modified);
    std::string& defined = g_defined[ed.hwnd()];
    if (defined == key) return;
    defined = key;
    ed.Call(SCI_RGBAIMAGESETWIDTH, w);
    ed.Call(SCI_RGBAIMAGESETHEIGHT, h);
    ed.Call(SCI_RGBAIMAGESETSCALE, 100);
    for (auto [change, color] : {std::pair{LineDiff::Change::Added, added}, std::pair{LineDiff::Change::Modified, modified}}) {
        const std::pair<Shape, std::pair<bool, bool>> shapes[] = {
            {Single, {true, true}}, {Top, {true, false}}, {Middle, {false, false}}, {Bottom, {false, true}}};
        for (auto [shape, round] : shapes) {
            std::vector<uint8_t> image = Image(w, h, color, round.first, round.second, h);
            ed.Call(SCI_MARKERDEFINERGBAIMAGE, MarkerOf(change, shape), reinterpret_cast<sptr_t>(image.data()));
        }
    }
    std::vector<uint8_t> image = Image(w, h, deleted, true, true, std::min(h, w * 2));
    ed.Call(SCI_MARKERDEFINERGBAIMAGE, MarkerOf(LineDiff::Change::Deleted, Single), reinterpret_cast<sptr_t>(image.data()));
}
}

namespace ChangeMarks {

void Attach(Editor* ed) {
    int mask = 0;
    for (int m = kFirstShape; m <= kFirstShape + 8; m++) mask |= 1 << m;
    ed->Call(SCI_SETMARGINMASKN, Editor::kChangeMargin, mask);
}

void Show(Editor& ed, const std::map<int, LineDiff::Change>& changes) {
    Define(ed);
    for (int m : {Editor::kMarkerAdded, Editor::kMarkerModified, Editor::kMarkerDeleted}) ed.Call(SCI_MARKERDELETEALL, m);
    for (int m = kFirstShape; m <= kFirstShape + 8; m++) ed.Call(SCI_MARKERDELETEALL, m);
    auto same = [&](int line, LineDiff::Change change) {
        auto it = changes.find(line);
        return it != changes.end() && it->second == change;
    };
    for (const auto& [line, change] : changes) {
        ed.Call(SCI_MARKERADD, line, change == LineDiff::Change::Added      ? Editor::kMarkerAdded
                                     : change == LineDiff::Change::Modified ? Editor::kMarkerModified
                                                                            : Editor::kMarkerDeleted);
        Shape shape = Single;
        if (change != LineDiff::Change::Deleted) {
            bool above = same(line - 1, change), below = same(line + 1, change);
            shape = above && below ? Middle : above ? Bottom : below ? Top : Single;
        }
        ed.Call(SCI_MARKERADD, line, MarkerOf(change, shape));
    }
}
}
