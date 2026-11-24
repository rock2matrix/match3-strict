#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

namespace m3 {

inline constexpr std::uint8_t empty = 255;

struct Board {
    int rows;
    int cols;
    int colors;
    std::vector<std::uint8_t> cells;

    Board(int height, int width, int color_count) : rows(height), cols(width), colors(color_count)
    {
        if (rows <= 0 || cols <= 0 || rows > std::numeric_limits<int>::max() / cols ||
            colors <= 0 || colors > empty) {
            throw std::invalid_argument("invalid board dimensions or color count");
        }
        cells.assign(rows * cols, empty);
    }

    bool operator==(const Board&) const = default;
};

using Matches = std::vector<int>;

struct Swap {
    int a;
    int b;

    bool operator==(const Swap&) const = default;
};

struct Falling {
    Board board;
    std::vector<int> holes;
    std::vector<int> changed;
};

namespace board_detail {

inline Matches marked_positions(const std::vector<std::uint8_t>& marked)
{
    Matches matches;
    for (int index = 0; index < static_cast<int>(marked.size()); ++index) {
        if (marked[index]) {
            matches.push_back(index);
        }
    }
    return matches;
}

inline void mark_runs(const Board& board, int start, int length, int stride,
                      std::vector<std::uint8_t>& marked)
{
    int begin = 0;
    while (begin < length) {
        const auto color = board.cells[start + begin * stride];
        int end = begin + 1;
        while (end < length && board.cells[start + end * stride] == color) {
            ++end;
        }
        if (color != empty && end - begin >= 3) {
            for (int offset = begin; offset < end; ++offset) {
                marked[start + offset * stride] = 1;
            }
        }
        begin = end;
    }
}

inline void mark_windows(const Board& board, int start, int length, int stride,
                         std::vector<std::uint8_t>& marked)
{
    for (int offset = 0; offset < length - 2; ++offset) {
        const int first = start + offset * stride;
        const int second = first + stride;
        const int third = second + stride;
        const auto color = board.cells[first];
        if (color != empty && color == board.cells[second] && color == board.cells[third]) {
            marked[first] = marked[second] = marked[third] = 1;
        }
    }
}

inline bool matches_at(const Board& board, int index)
{
    const auto color = board.cells[index];
    const int row = index / board.cols;
    const int col = index % board.cols;
    const int stride = board.cols;
    return (col >= 2 && board.cells[index - 1] == color && board.cells[index - 2] == color) ||
           (col >= 1 && col < board.cols - 1 && board.cells[index - 1] == color &&
            board.cells[index + 1] == color) ||
           (col < board.cols - 2 && board.cells[index + 1] == color && board.cells[index + 2] == color) ||
           (row >= 2 && board.cells[index - stride] == color && board.cells[index - 2 * stride] == color) ||
           (row >= 1 && row < board.rows - 1 && board.cells[index - stride] == color &&
            board.cells[index + stride] == color) ||
           (row < board.rows - 2 && board.cells[index + stride] == color &&
            board.cells[index + 2 * stride] == color);
}

template <class Accept>
std::vector<Swap> collect_swaps(const Board& original, Accept accept)
{
    Board board = original;
    std::vector<Swap> swaps;
    const auto test = [&](int a, int b) {
        if (board.cells[a] == board.cells[b] || board.cells[a] == empty || board.cells[b] == empty) {
            return;
        }
        std::swap(board.cells[a], board.cells[b]);
        if (accept(board, a, b)) {
            swaps.push_back({a, b});
        }
        std::swap(board.cells[a], board.cells[b]);
    };
    for (int row = 0; row < board.rows; ++row) {
        for (int col = 0; col < board.cols; ++col) {
            const int a = row * board.cols + col;
            if (col + 1 < board.cols) {
                test(a, a + 1);
            }
            if (row + 1 < board.rows) {
                test(a, a + board.cols);
            }
        }
    }
    return swaps;
}

inline int uniform_below(std::mt19937_64& random, std::uint64_t bound)
{
    const auto threshold = (std::uint64_t{0} - bound) % bound;
    auto value = random();
    while (value < threshold) {
        value = random();
    }
    return static_cast<int>(value % bound);
}

} // namespace board_detail

inline Matches full_matches(const Board& board)
{
    std::vector<std::uint8_t> marked(board.cells.size());
    for (int row = 0; row < board.rows; ++row) {
        board_detail::mark_runs(board, row * board.cols, board.cols, 1, marked);
    }
    for (int col = 0; col < board.cols; ++col) {
        board_detail::mark_runs(board, col, board.rows, board.cols, marked);
    }
    return board_detail::marked_positions(marked);
}

// Any remaining match must contain a changed cell. Before the edits the board
// was stable, or all previously matching cells were included in the edits.
inline Matches dirty_matches(const Board& board, const std::vector<int>& changed)
{
    std::vector<std::uint8_t> marked(board.cells.size());
    std::vector<std::uint8_t> dirty_rows(board.rows);
    std::vector<std::uint8_t> dirty_cols(board.cols);
    for (const int index : changed) {
        dirty_rows[index / board.cols] = 1;
        dirty_cols[index % board.cols] = 1;
    }
    for (int row = 0; row < board.rows; ++row) {
        if (dirty_rows[row]) {
            board_detail::mark_windows(board, row * board.cols, board.cols, 1, marked);
        }
    }
    for (int col = 0; col < board.cols; ++col) {
        if (dirty_cols[col]) {
            board_detail::mark_windows(board, col, board.rows, board.cols, marked);
        }
    }
    return board_detail::marked_positions(marked);
}

// On a stable board any newly created match passes through a swap endpoint.
inline std::vector<Swap> legal_swaps(const Board& board)
{
    return board_detail::collect_swaps(board, [](const Board& swapped, int a, int b) {
        return board_detail::matches_at(swapped, a) || board_detail::matches_at(swapped, b);
    });
}

inline std::vector<Swap> reference_swaps(const Board& board)
{
    return board_detail::collect_swaps(board, [](const Board& swapped, int, int) {
        return !full_matches(swapped).empty();
    });
}

inline Falling clear_and_drop(const Board& board, const Matches& matches)
{
    Falling falling{board, {}, {}};
    std::vector<std::uint8_t> cleared(board.cells.size());
    for (const int index : matches) {
        cleared[index] = 1;
        falling.board.cells[index] = empty;
    }
    auto& cells = falling.board.cells;
    for (int col = 0; col < board.cols; ++col) {
        int write_row = board.rows - 1;
        for (int read_row = board.rows - 1; read_row >= 0; --read_row) {
            const auto color = cells[read_row * board.cols + col];
            if (color != empty) {
                cells[write_row * board.cols + col] = color;
                --write_row;
            }
        }
        for (int row = 0; row <= write_row; ++row) {
            const int index = row * board.cols + col;
            cells[index] = empty;
            cleared[index] = 1;
            falling.holes.push_back(index);
        }
    }
    for (int index = 0; index < static_cast<int>(cells.size()); ++index) {
        if (cleared[index] || cells[index] != board.cells[index]) {
            falling.changed.push_back(index);
        }
    }
    return falling;
}

inline Board random_stable(int rows, int cols, int colors, std::uint64_t seed)
{
    Board board(rows, cols, colors);
    if (colors < 3) {
        throw std::invalid_argument("stable-board generation requires at least three colors");
    }
    std::mt19937_64 random(seed);
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < cols; ++col) {
            const int index = row * cols + col;
            int excluded_left = empty;
            int excluded_up = empty;
            if (col >= 2 && board.cells[index - 1] == board.cells[index - 2]) {
                excluded_left = board.cells[index - 1];
            }
            if (row >= 2 && board.cells[index - cols] == board.cells[index - 2 * cols]) {
                excluded_up = board.cells[index - cols];
            }
            if (excluded_left == excluded_up) {
                excluded_up = empty;
            }
            const int options = colors - (excluded_left != empty) - (excluded_up != empty);
            int color = board_detail::uniform_below(random, options);
            color += color >= std::min(excluded_left, excluded_up);
            color += color >= std::max(excluded_left, excluded_up);
            board.cells[index] = static_cast<std::uint8_t>(color);
        }
    }
    return board;
}

} // namespace m3
