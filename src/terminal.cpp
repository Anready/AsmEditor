#include "terminal.h"

#include <algorithm>

namespace {
void AppendUtf8(std::string& out, char32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

constexpr char kEsc = '\x1B';
}

Terminal::Terminal(int cols, int rows) : cols_(std::max(cols, 2)), rows_(std::max(rows, 2)) {
    grid_.assign(rows_, BlankRow());
    scrollBottom_ = rows_ - 1;
}

// ---- input parsing ----------------------------------------------------------------------------------------------

void Terminal::Feed(const char* data, size_t size) {
    for (size_t i = 0; i < size; i++) {
        unsigned char b = static_cast<unsigned char>(data[i]);
        switch (state_) {
        case State::Ground:
            if (utf8Remaining_) {
                if ((b & 0xC0) == 0x80) {
                    utf8Code_ = (utf8Code_ << 6) | (b & 0x3F);
                    if (--utf8Remaining_ == 0) Put(utf8Code_);
                    break;
                }
                utf8Remaining_ = 0;  // broken sequence: drop it and handle this byte normally
                Put(U'?');
            }
            if (b >= 0xF0) {
                utf8Code_ = b & 0x07;
                utf8Remaining_ = 3;
            } else if (b >= 0xE0) {
                utf8Code_ = b & 0x0F;
                utf8Remaining_ = 2;
            } else if (b >= 0xC0) {
                utf8Code_ = b & 0x1F;
                utf8Remaining_ = 1;
            } else if (b >= 0x80) {
                Put(U'?');
            } else if (b == 0x1B) {
                state_ = State::Escape;
            } else if (b < 0x20 || b == 0x7F) {
                Control(b);
            } else {
                Put(b);
            }
            break;
        case State::Escape:
            if (b == '[') {
                state_ = State::Csi;
                params_.clear();
                current_ = -1;
                marker_ = 0;
            } else if (b == ']') {
                state_ = State::Osc;
            } else if (b == '(' || b == ')') {
                state_ = State::Charset;
            } else {
                state_ = State::Ground;
                EscapeFinal(b);
            }
            break;
        case State::Charset:
            state_ = State::Ground;
            break;
        case State::Osc:  // window title etc.: ignored until BEL or ESC '\'
            if (b == 0x07) state_ = State::Ground;
            else if (b == 0x1B) state_ = State::OscEscape;
            break;
        case State::OscEscape:
            state_ = b == '\\' ? State::Ground : State::Osc;
            break;
        case State::Csi:
            if (b >= '0' && b <= '9') {
                current_ = std::min((current_ < 0 ? 0 : current_) * 10 + (b - '0'), 100000);
            } else if (b == ';' || b == ':') {
                params_.push_back(current_);
                current_ = -1;
            } else if (b >= '<' && b <= '?') {
                marker_ = static_cast<char>(b);
            } else if (b >= 0x40 && b <= 0x7E) {
                params_.push_back(current_);
                state_ = State::Ground;
                CsiFinal(b);
            } else if (b == 0x1B) {
                state_ = State::Escape;
            }
            break;
        }
    }
}

int Terminal::Param(size_t i, int def) const {
    return i < params_.size() && params_[i] > 0 ? params_[i] : def;
}

void Terminal::Control(unsigned char c) {
    switch (c) {
    case '\r':
        col_ = 0;
        wrapPending_ = false;
        break;
    case '\n':
    case '\v':
    case '\f':
        wrapPending_ = false;
        LineFeed();
        break;
    case '\b':
        if (col_ > 0) col_--;
        wrapPending_ = false;
        break;
    case '\t':
        col_ = std::min(cols_ - 1, (col_ / 8 + 1) * 8);
        break;
    default:
        break;
    }
}

void Terminal::EscapeFinal(unsigned char c) {
    switch (c) {
    case '7':
        savedRow_ = row_;
        savedCol_ = col_;
        break;
    case '8':
        row_ = savedRow_;
        col_ = savedCol_;
        wrapPending_ = false;
        break;
    case 'D':
        LineFeed();
        break;
    case 'E':
        col_ = 0;
        LineFeed();
        break;
    case 'M':  // reverse index
        if (row_ == scrollTop_) Scroll(scrollTop_, scrollBottom_, 1, false, false);
        else if (row_ > 0) row_--;
        break;
    case 'c':  // full reset
        grid_.assign(rows_, BlankRow());
        row_ = col_ = 0;
        style_ = 0;
        fg_ = -1;
        bold_ = false;
        scrollTop_ = 0;
        scrollBottom_ = rows_ - 1;
        break;
    default:
        break;
    }
}

void Terminal::CsiFinal(unsigned char c) {
    int n = Param(0, 1);
    int mode = params_.empty() || params_[0] < 0 ? 0 : params_[0];
    if (c != 'm' && c != 'n' && c != 'c' && c != 'h' && c != 'l') wrapPending_ = false;
    switch (c) {
    case 'A': row_ = std::max(0, row_ - n); break;
    case 'B': row_ = std::min(rows_ - 1, row_ + n); break;
    case 'C': col_ = std::min(cols_ - 1, col_ + n); break;
    case 'D': col_ = std::max(0, col_ - n); break;
    case 'E': row_ = std::min(rows_ - 1, row_ + n); col_ = 0; break;
    case 'F': row_ = std::max(0, row_ - n); col_ = 0; break;
    case 'G':
    case '`': col_ = std::clamp(n - 1, 0, cols_ - 1); break;
    case 'd': row_ = std::clamp(n - 1, 0, rows_ - 1); break;
    case 'H':
    case 'f':
        row_ = std::clamp(Param(0, 1) - 1, 0, rows_ - 1);
        col_ = std::clamp(Param(1, 1) - 1, 0, cols_ - 1);
        break;
    case 'J':
        if (mode == 0) {
            ClearCells(row_, col_, cols_);
            for (int r = row_ + 1; r < rows_; r++) grid_[r] = BlankRow();
        } else if (mode == 1) {
            for (int r = 0; r < row_; r++) grid_[r] = BlankRow();
            ClearCells(row_, 0, col_ + 1);
        } else {
            for (Row& r : grid_) r = BlankRow();
        }
        break;
    case 'K':
        if (mode == 0) ClearCells(row_, col_, cols_);
        else if (mode == 1) ClearCells(row_, 0, col_ + 1);
        else ClearCells(row_, 0, cols_);
        break;
    case 'X': ClearCells(row_, col_, std::min(cols_, col_ + n)); break;
    case 'P': {
        Row& r = grid_[row_];
        int k = std::min(n, cols_ - col_);
        r.erase(r.begin() + col_, r.begin() + col_ + k);
        r.insert(r.end(), k, Cell{});
        break;
    }
    case '@': {
        Row& r = grid_[row_];
        int k = std::min(n, cols_ - col_);
        r.insert(r.begin() + col_, k, Cell{});
        r.resize(cols_);
        break;
    }
    case 'L':
        if (row_ >= scrollTop_ && row_ <= scrollBottom_) Scroll(row_, scrollBottom_, n, false, false);
        break;
    case 'M':
        if (row_ >= scrollTop_ && row_ <= scrollBottom_) Scroll(row_, scrollBottom_, n, true, false);
        break;
    case 'S': Scroll(scrollTop_, scrollBottom_, n, true, scrollTop_ == 0); break;
    case 'T': Scroll(scrollTop_, scrollBottom_, n, false, false); break;
    case 'm': Sgr(); break;
    case 'r':
        if (!marker_) {
            int top = Param(0, 1) - 1, bottom = Param(1, rows_) - 1;
            if (top < bottom && bottom < rows_) {
                scrollTop_ = top;
                scrollBottom_ = bottom;
            } else {
                scrollTop_ = 0;
                scrollBottom_ = rows_ - 1;
            }
            row_ = col_ = 0;
        }
        break;
    case 's':
        savedRow_ = row_;
        savedCol_ = col_;
        break;
    case 'u':
        row_ = savedRow_;
        col_ = savedCol_;
        break;
    case 'n':  // device status report
        if (mode == 6) {
            response_ += kEsc;
            response_ += "[" + std::to_string(row_ + 1) + ";" + std::to_string(col_ + 1) + "R";
        } else if (mode == 5) {
            response_ += kEsc;
            response_ += "[0n";
        }
        break;
    case 'c':  // device attributes: claim a VT100 with advanced video
        if (!marker_) {
            response_ += kEsc;
            response_ += "[?1;0c";
        }
        break;
    default:  // modes (h/l), window ops etc. are not needed for a console
        break;
    }
}

void Terminal::Sgr() {
    for (size_t i = 0; i < params_.size(); i++) {
        int p = params_[i] < 0 ? 0 : params_[i];
        if (p == 0) {
            fg_ = -1;
            bold_ = false;
        } else if (p == 1) {
            bold_ = true;
        } else if (p == 22) {
            bold_ = false;
        } else if (p >= 30 && p <= 37) {
            fg_ = p - 30;
        } else if (p >= 90 && p <= 97) {
            fg_ = p - 90 + 8;
        } else if (p == 39) {
            fg_ = -1;
        } else if (p == 38 || p == 48) {  // extended colors: 256-color index or RGB
            int kind = i + 1 < params_.size() ? params_[i + 1] : 0;
            if (kind == 5) {
                int idx = i + 2 < params_.size() ? params_[i + 2] : 0;
                if (p == 38) fg_ = idx >= 0 && idx < 16 ? idx : -1;
                i += 2;
            } else if (kind == 2) {
                if (p == 38) fg_ = -1;
                i += 4;
            }
        }
    }
    style_ = fg_ < 0 ? 0 : static_cast<uint8_t>(kStyleBase + ((bold_ && fg_ < 8) ? fg_ + 8 : fg_));
}

// ---- screen operations ------------------------------------------------------------------------------------------

void Terminal::Touch(int row) {
    if (row < top_) top_ = row;
}

void Terminal::Put(char32_t ch) {
    if (wrapPending_) {
        wrapPending_ = false;
        col_ = 0;
        LineFeed();
    }
    grid_[row_][col_] = {ch, style_};
    Touch(row_);
    if (col_ == cols_ - 1) wrapPending_ = true;
    else col_++;
}

void Terminal::LineFeed() {
    if (row_ == scrollBottom_) Scroll(scrollTop_, scrollBottom_, 1, true, scrollTop_ == 0);
    else if (row_ < rows_ - 1) row_++;
}

void Terminal::Scroll(int top, int bottom, int count, bool up, bool toHistory) {
    count = std::min(count, bottom - top + 1);
    for (int k = 0; k < count; k++) {
        if (up) {
            if (toHistory && top == 0) {
                if (top_ == 0) history_.push_back(grid_[0]);  // committed rows were already handed out
                else top_--;
            }
            grid_.erase(grid_.begin() + top);
            grid_.insert(grid_.begin() + bottom, BlankRow());
        } else {
            grid_.erase(grid_.begin() + bottom);
            grid_.insert(grid_.begin() + top, BlankRow());
        }
    }
}

void Terminal::ClearCells(int row, int from, int to) {
    for (int c = std::max(0, from); c < std::min(to, cols_); c++) grid_[row][c] = Cell{};
}

void Terminal::Resize(int cols, int rows) {
    cols = std::max(cols, 2);
    rows = std::max(rows, 2);
    if (cols == cols_ && rows == rows_) return;
    // Keep the cursor on screen: move rows that no longer fit above it into history.
    while (row_ >= rows) {
        if (top_ == 0) history_.push_back(grid_[0]);
        else top_--;
        grid_.erase(grid_.begin());
        grid_.push_back(BlankRow());
        row_--;
    }
    cols_ = cols;
    for (Row& r : grid_) r.resize(cols_);
    grid_.resize(rows, BlankRow());
    rows_ = rows;
    col_ = std::min(col_, cols_ - 1);
    scrollTop_ = 0;
    scrollBottom_ = rows_ - 1;
    top_ = std::min(top_, row_);
}

int Terminal::LastUsedRow() const {
    for (int r = rows_ - 1; r > row_; r--) {
        for (const Cell& c : grid_[r]) {
            if (c.ch != U' ') return r;
        }
    }
    return row_;
}

void Terminal::Commit() {
    int last = LastUsedRow();
    for (int r = std::min(top_, row_); r <= last; r++) {
        history_.push_back(grid_[r]);
        grid_[r] = BlankRow();
    }
    top_ = row_;
}

// ---- output -----------------------------------------------------------------------------------------------------

TermLine Terminal::MakeLine(const Row& row, int minCols, size_t* cursorByte) const {
    int len = static_cast<int>(row.size());
    while (len > 0 && row[len - 1].ch == U' ') len--;
    len = std::max(len, minCols);
    TermLine line;
    for (int c = 0; c < len; c++) {
        if (cursorByte && c == minCols) *cursorByte = line.text.size();
        size_t before = line.text.size();
        const Cell cell = c < static_cast<int>(row.size()) ? row[c] : Cell{};
        AppendUtf8(line.text, cell.ch);
        int bytes = static_cast<int>(line.text.size() - before);
        if (!line.runs.empty() && line.runs.back().second == cell.style) line.runs.back().first += bytes;
        else line.runs.push_back({bytes, cell.style});
    }
    if (cursorByte && minCols >= len) *cursorByte = line.text.size();
    return line;
}

std::vector<TermLine> Terminal::TakeHistory() {
    std::vector<TermLine> out;
    out.reserve(history_.size());
    for (const Row& r : history_) out.push_back(MakeLine(r, 0, nullptr));
    history_.clear();
    return out;
}

std::vector<TermLine> Terminal::Screen(size_t& cursorOffset) const {
    std::vector<TermLine> out;
    int last = LastUsedRow();
    size_t offset = 0;
    for (int r = std::min(top_, row_); r <= last; r++) {
        size_t cursorByte = 0;
        bool isCursorRow = r == row_;
        out.push_back(MakeLine(grid_[r], isCursorRow ? col_ : 0, isCursorRow ? &cursorByte : nullptr));
        if (isCursorRow) cursorOffset = offset + cursorByte;
        offset += out.back().text.size() + 1;
    }
    return out;
}

std::string Terminal::TakeResponse() {
    std::string r;
    r.swap(response_);
    return r;
}
