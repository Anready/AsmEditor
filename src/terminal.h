#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// One rendered line: UTF-8 text plus style runs (byte length, style).
struct TermLine {
    std::string text;
    std::vector<std::pair<int, int>> runs;
};

// Minimal VT/ANSI terminal emulator for the output of a Windows pseudoconsole (ConPTY).
// Keeps a screen grid; lines scrolled off the top are handed out once as history.
class Terminal {
public:
    static constexpr int kStyleBase = 16;  // style of ANSI color 0; style 0 is the default color

    Terminal(int cols = 80, int rows = 25);

    void Feed(const char* data, size_t size);
    void Resize(int cols, int rows);
    // Moves everything printed so far into history, so other text can be appended below it.
    void Commit();

    std::vector<TermLine> TakeHistory();
    // Visible lines (joined with '\n' by the caller); cursorOffset = byte offset of the cursor in the joined text.
    std::vector<TermLine> Screen(size_t& cursorOffset) const;
    std::string TakeResponse();  // replies to queries (cursor position etc.) to send back to the program

    int cols() const { return cols_; }
    int rows() const { return rows_; }

private:
    struct Cell {
        char32_t ch = U' ';
        uint8_t style = 0;
    };
    using Row = std::vector<Cell>;
    enum class State { Ground, Escape, Csi, Osc, OscEscape, Charset };

    void Put(char32_t ch);
    void Control(unsigned char c);
    void EscapeFinal(unsigned char c);
    void CsiFinal(unsigned char c);
    void Sgr();
    void LineFeed();
    void Scroll(int top, int bottom, int count, bool up, bool toHistory);
    void ClearCells(int row, int from, int to);
    void Touch(int row);
    int Param(size_t i, int def) const;
    int LastUsedRow() const;
    TermLine MakeLine(const Row& row, int minCols, size_t* cursorByte) const;
    Row BlankRow() const { return Row(cols_); }

    int cols_, rows_;
    std::vector<Row> grid_;
    std::vector<Row> history_;
    int row_ = 0, col_ = 0;
    bool wrapPending_ = false;
    int savedRow_ = 0, savedCol_ = 0;
    int scrollTop_ = 0, scrollBottom_ = 0;
    int top_ = 0;  // rows above this were committed (they are blank and not shown)
    uint8_t style_ = 0;
    int fg_ = -1;
    bool bold_ = false;

    State state_ = State::Ground;
    std::vector<int> params_;
    int current_ = -1;
    char marker_ = 0;
    char32_t utf8Code_ = 0;
    int utf8Remaining_ = 0;
    std::string response_;
};
