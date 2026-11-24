#pragma once

#include "refill.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <istream>
#include <limits>
#include <numeric>
#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace m3 {

enum class CursorResult { candidate, exhausted, paused };

class RefillCursor {
    using Mask = refill_detail::Mask;
    using Pair = refill_detail::Pair;
    using Triplet = refill_detail::Triplet;
    static constexpr int max_colors = refill_detail::max_colors;
    static constexpr int max_board_cells = 1 << 20;

    struct Branch {
        std::size_t slot{};
        Mask choices{};
    };

    struct Frame {
        Branch branch;
        bool entered{};
    };

    struct Assignment {
        std::size_t slot;
        int color;
    };

public:
    RefillCursor(const Board& partial, const std::vector<int>& holes,
                 const std::vector<int>& quota, RefillMode mode,
                 bool require_stable, bool ranked)
        : board_(partial), holes_(holes), quota_(quota), remaining_(quota), mode_(mode),
          stable_(require_stable), ranked_(ranked), values_(holes.size(), empty),
          contacts_(holes.size()), priority_(holes.size())
    {
        if (board_.rows < 1 || board_.cols < 1 || board_.rows > max_board_cells / board_.cols ||
            board_.cells.size() != static_cast<std::size_t>(board_.rows * board_.cols)) {
            throw std::invalid_argument("invalid refill cursor board dimensions");
        }
        if (board_.colors < 1 || board_.colors > max_colors ||
            quota.size() != static_cast<std::size_t>(board_.colors)) {
            throw std::invalid_argument("invalid refill cursor color count");
        }
        if (mode_ != RefillMode::raw && mode_ != RefillMode::quota && mode_ != RefillMode::csp) {
            throw std::invalid_argument("invalid refill cursor mode");
        }
        if (std::ranges::any_of(quota, [](int count) { return count < 0; }) ||
            std::accumulate(quota.begin(), quota.end(), std::int64_t{0}) !=
                static_cast<std::int64_t>(holes_.size())) {
            throw std::invalid_argument("refill cursor quota must sum to the number of holes");
        }

        std::vector<int> slot_by_cell(board_.cells.size(), -1);
        std::vector<int> holes_per_column(board_.cols);
        for (std::size_t slot = 0; slot < holes_.size(); ++slot) {
            const int cell = holes_[slot];
            if (cell < 0 || static_cast<std::size_t>(cell) >= board_.cells.size() ||
                board_.cells[cell] != empty || slot_by_cell[cell] >= 0) {
                throw std::invalid_argument("invalid refill cursor hole");
            }
            slot_by_cell[cell] = static_cast<int>(slot);
            ++holes_per_column[cell % board_.cols];
        }
        if (std::ranges::count(board_.cells, empty) != static_cast<std::ptrdiff_t>(holes_.size()) ||
            std::ranges::any_of(board_.cells, [&](auto color) {
                return color != empty && color >= board_.colors;
            })) {
            throw std::invalid_argument("invalid refill cursor cells");
        }

        if (stable_ || ranked_) {
            for (int row = 0; row < board_.rows; ++row) {
                for (int col = 0; col < board_.cols; ++col) {
                    const int cell = row * board_.cols + col;
                    if (col + 2 < board_.cols) {
                        add_window({cell, cell + 1, cell + 2}, slot_by_cell);
                    }
                    if (row + 2 < board_.rows) {
                        add_window({cell, cell + board_.cols, cell + 2 * board_.cols},
                                   slot_by_cell);
                    }
                }
            }
            for (std::size_t slot = 0; slot < holes_.size(); ++slot) {
                priority_[slot] = {
                    static_cast<int>(contacts_[slot].size()),
                    holes_per_column[holes_[slot] % board_.cols]
                };
            }
        }
        frames_.reserve(holes_.size() + 1);
        trail_.reserve(holes_.size());
        frames_.emplace_back();
    }

    CursorResult next(RefillStats& stats, std::uint64_t absolute_node_limit)
    {
        while (!frames_.empty()) {
            auto& frame = frames_.back();
            if (!frame.entered) {
                if (stats.nodes >= absolute_node_limit) {
                    ++stats.node_cutoffs;
                    return CursorResult::paused;
                }
                ++stats.nodes;
                frame.entered = true;
                if (enter(frame, stats)) {
                    return CursorResult::candidate;
                }
            }

            if (frame.branch.choices == 0) {
                frames_.pop_back();
                if (!trail_.empty()) {
                    undo();
                }
                continue;
            }

            const int color = take_color(frame.branch);
            assign(frame.branch.slot, color);
            frames_.emplace_back();
        }
        return CursorResult::exhausted;
    }

    // Valid after next() returns candidate, until the cursor advances again.
    const std::vector<std::uint8_t>& values() const { return values_; }

    bool matches_problem(const Board& partial, const std::vector<int>& holes,
                         const std::vector<int>& quota, RefillMode mode,
                         bool stable, bool ranked) const
    {
        if (holes != holes_ || quota != quota_ || mode != mode_ || stable != stable_ || ranked != ranked_) {
            return false;
        }
        auto original = board_;
        for (const int cell : holes_) {
            original.cells[cell] = empty;
        }
        return original == partial;
    }

    void write(std::ostream& output) const;
    static RefillCursor read(std::istream& input);

private:
    static std::string read_token(std::istream& input)
    {
        std::string token;
        if (!(input >> std::setw(32) >> token) || token.size() >= 32) {
            throw std::runtime_error("invalid refill cursor token");
        }
        return token;
    }

    template<class Integer>
    static Integer read_number(std::istream& input)
    {
        const auto token = read_token(input);
        Integer result{};
        const auto end = token.data() + token.size();
        const auto [parsed, error] = std::from_chars(token.data(), end, result);
        if (error != std::errc{} || parsed != end) {
            throw std::runtime_error("invalid refill cursor number");
        }
        return result;
    }

    static bool read_flag(std::istream& input)
    {
        const auto flag = read_number<unsigned>(input);
        if (flag > 1) {
            throw std::runtime_error("invalid refill cursor flag");
        }
        return flag != 0;
    }

    bool csp_stable() const { return mode_ == RefillMode::csp && stable_; }

    void add_window(const Triplet& window, const std::vector<int>& slot_by_cell)
    {
        bool touches_hole = false;
        for (int i = 0; i < 3; ++i) {
            const int slot = slot_by_cell[window[i]];
            if (slot >= 0) {
                contacts_[slot].push_back({window[(i + 1) % 3], window[(i + 2) % 3]});
                touches_hole = true;
            }
        }
        if (touches_hole) {
            windows_.push_back(window);
        } else if (is_match(window)) {
            fixed_match_ = true;
        }
    }

    bool is_match(const Triplet& window) const
    {
        const auto color = board_.cells[window[0]];
        return color != empty && color == board_.cells[window[1]] &&
               color == board_.cells[window[2]];
    }

    bool quota_exact() const
    {
        return std::ranges::all_of(remaining_, [](int count) { return count == 0; });
    }

    bool stable_board() const
    {
        return !fixed_match_ && std::ranges::none_of(windows_, [this](const Triplet& window) {
            return is_match(window);
        });
    }

    bool enter(Frame& frame, RefillStats& stats)
    {
        if (csp_stable() && fixed_match_) {
            ++stats.pruned;
            ++stats.infeasible_domains;
            return false;
        }
        if (trail_.size() == holes_.size()) {
            ++stats.leaves;
            if (mode_ == RefillMode::raw && !quota_exact()) {
                ++stats.pruned;
                ++stats.quota_rejected_leaves;
                return false;
            }
            if (stable_ && !csp_stable() && !stable_board()) {
                ++stats.pruned;
                ++stats.unstable_leaves;
                return false;
            }
            ++stats.emitted;
            return true;
        }

        frame.branch = branch(trail_.size());
        if (csp_stable() && frame.branch.choices == 0) {
            ++stats.pruned;
            ++stats.infeasible_domains;
        } else if (mode_ != RefillMode::raw) {
            stats.pruned += board_.colors - std::popcount(frame.branch.choices);
        }
        return false;
    }

    void assign(std::size_t slot, int color)
    {
        --remaining_[color];
        values_[slot] = static_cast<std::uint8_t>(color);
        board_.cells[holes_[slot]] = values_[slot];
        trail_.push_back({slot, color});
    }

    void undo()
    {
        const auto [slot, color] = trail_.back();
        trail_.pop_back();
        ++remaining_[color];
        values_[slot] = empty;
        board_.cells[holes_[slot]] = empty;
    }

    Mask all_colors() const
    {
        return board_.colors == max_colors ? std::numeric_limits<Mask>::max()
                                         : (Mask{1} << board_.colors) - 1;
    }

    Mask remaining_colors() const
    {
        Mask mask = 0;
        for (int color = 0; color < board_.colors; ++color) {
            if (remaining_[color] > 0) {
                mask |= Mask{1} << color;
            }
        }
        return mask;
    }

    int local_score(std::size_t slot, int color) const
    {
        int score = 0;
        for (const auto& [left, right] : contacts_[slot]) {
            const auto a = board_.cells[left];
            const auto b = board_.cells[right];
            if (a == color && b == color) {
                score += 3;
            } else if ((a == color && b == empty) || (a == empty && b == color)) {
                ++score;
            }
        }
        return score;
    }

    int best_color(std::size_t slot, Mask choices) const
    {
        int best = std::countr_zero(choices);
        if (!ranked_ || mode_ == RefillMode::raw) {
            return best;
        }
        std::array best_score{local_score(slot, best), remaining_[best]};
        for (choices &= choices - 1; choices != 0; choices &= choices - 1) {
            const int color = std::countr_zero(choices);
            const std::array score{local_score(slot, color), remaining_[color]};
            if (score > best_score) {
                best = color;
                best_score = score;
            }
        }
        return best;
    }

    int take_color(Branch& branch) const
    {
        const int color = best_color(branch.slot, branch.choices);
        branch.choices ^= Mask{1} << color;
        return color;
    }

    Mask legal_colors(std::size_t slot, Mask choices) const
    {
        for (const auto& [left, right] : contacts_[slot]) {
            const auto color = board_.cells[left];
            if (color != empty && color == board_.cells[right]) {
                choices &= ~(Mask{1} << color);
            }
        }
        return choices;
    }

    Branch branch(std::size_t depth) const
    {
        if (mode_ == RefillMode::raw) {
            return {depth, all_colors()};
        }
        const Mask available = remaining_colors();
        if (!csp_stable()) {
            return {depth, available};
        }

        std::array<int, max_colors> support{};
        Branch selected;
        int best_size = max_colors + 1;
        for (std::size_t slot = 0; slot < holes_.size(); ++slot) {
            if (board_.cells[holes_[slot]] != empty) {
                continue;
            }
            const Mask legal = legal_colors(slot, available);
            const int size = std::popcount(legal);
            if (size == 0) {
                return {};
            }
            if (size < best_size || (size == best_size && priority_[slot] > priority_[selected.slot])) {
                selected = {slot, legal};
                best_size = size;
            }
            for (Mask supported = legal; supported != 0; supported &= supported - 1) {
                ++support[std::countr_zero(supported)];
            }
        }
        for (Mask colors = available; colors != 0; colors &= colors - 1) {
            const int color = std::countr_zero(colors);
            if (support[color] < remaining_[color]) {
                return {};
            }
        }
        for (Mask colors = selected.choices; colors != 0; colors &= colors - 1) {
            const int color = std::countr_zero(colors);
            if (support[color] == remaining_[color]) {
                selected.choices &= Mask{1} << color;
            }
        }
        return selected;
    }

    Board board_;
    std::vector<int> holes_;
    std::vector<int> quota_;
    std::vector<int> remaining_;
    RefillMode mode_;
    bool stable_;
    bool ranked_;
    std::vector<std::uint8_t> values_;
    std::vector<std::vector<Pair>> contacts_;
    std::vector<std::array<int, 2>> priority_;
    std::vector<Triplet> windows_;
    bool fixed_match_{};
    std::vector<Frame> frames_;
    std::vector<Assignment> trail_;
};

inline void RefillCursor::write(std::ostream& output) const
{
    auto original_cells = board_.cells;
    for (const int cell : holes_) {
        original_cells[cell] = empty;
    }
    output << "M3_REFILL_CURSOR 1\n";
    output << board_.rows << ' ' << board_.cols << ' ' << board_.colors << ' '
           << static_cast<int>(mode_) << ' ' << static_cast<int>(stable_) << ' '
           << static_cast<int>(ranked_) << '\n';
    for (const auto color : original_cells) {
        output << static_cast<int>(color) << ' ';
    }
    output << '\n' << holes_.size();
    for (const int cell : holes_) {
        output << ' ' << cell;
    }
    output << '\n';
    for (const int count : quota_) {
        output << count << ' ';
    }

    // Every nonfrontier frame has one active assignment in the trail. The
    // saved color mask contains the siblings left after that assignment.
    output << '\n' << frames_.size() << '\n';
    for (std::size_t depth = 0; depth < frames_.size(); ++depth) {
        const auto& frame = frames_[depth];
        output << static_cast<int>(frame.entered) << ' ' << frame.branch.slot << ' '
               << frame.branch.choices;
        if (depth < trail_.size()) {
            output << ' ' << trail_[depth].color;
        }
        output << '\n';
    }
    output << "END_REFILL_CURSOR\n";
    if (!output) {
        throw std::runtime_error("could not write refill cursor");
    }
}

inline RefillCursor RefillCursor::read(std::istream& input)
{
    if (read_token(input) != "M3_REFILL_CURSOR" || read_number<unsigned>(input) != 1) {
        throw std::runtime_error("unsupported refill cursor format");
    }
    const auto rows = read_number<int>(input);
    const auto cols = read_number<int>(input);
    const auto colors = read_number<int>(input);
    const auto mode = read_number<unsigned>(input);
    const auto stable = read_flag(input);
    const auto ranked = read_flag(input);
    if (rows < 1 || cols < 1 || rows > max_board_cells / cols || colors < 1 || colors > max_colors ||
        mode > static_cast<unsigned>(RefillMode::csp)) {
        throw std::runtime_error("invalid serialized refill cursor board or mode");
    }
    Board partial(rows, cols, colors);
    for (auto& cell : partial.cells) {
        const auto color = read_number<unsigned>(input);
        if (color != empty && color >= static_cast<unsigned>(colors)) {
            throw std::runtime_error("invalid serialized refill cursor cell");
        }
        cell = static_cast<std::uint8_t>(color);
    }

    const auto hole_count = read_number<std::size_t>(input);
    if (hole_count > partial.cells.size()) {
        throw std::runtime_error("invalid serialized refill cursor hole count");
    }
    std::vector<int> holes(hole_count);
    for (auto& hole : holes) {
        hole = read_number<int>(input);
    }
    std::vector<int> quota(colors);
    for (auto& count : quota) {
        count = read_number<int>(input);
    }
    RefillCursor cursor(partial, holes, quota, static_cast<RefillMode>(mode), stable, ranked);

    const auto frame_count = read_number<std::size_t>(input);
    if (frame_count > hole_count + 1) {
        throw std::runtime_error("invalid serialized refill cursor frame count");
    }
    cursor.frames_.clear();
    for (std::size_t depth = 0; depth < frame_count; ++depth) {
        const auto entered = read_flag(input);
        const auto slot = read_number<std::size_t>(input);
        const auto choices = read_number<Mask>(input);
        if (depth + 1 < frame_count) {
            const auto assigned = read_number<int>(input);
            if (!entered || assigned < 0 || assigned >= colors ||
                (cursor.csp_stable() && cursor.fixed_match_)) {
                throw std::runtime_error("invalid serialized refill cursor assignment");
            }
            auto expected = cursor.branch(depth);
            if (slot != expected.slot || !(expected.choices & (Mask{1} << assigned))) {
                throw std::runtime_error("invalid serialized refill cursor branch");
            }
            int examined;
            do {
                examined = cursor.take_color(expected);
            } while (examined != assigned);
            if (choices != expected.choices) {
                throw std::runtime_error("invalid serialized refill cursor sibling mask");
            }
            cursor.assign(slot, assigned);
        } else if (slot != 0 || choices != 0 ||
                   (entered && (depth != hole_count || !cursor.quota_exact() ||
                                (stable && !cursor.stable_board())))) {
            throw std::runtime_error("invalid serialized refill cursor frontier");
        }
        cursor.frames_.push_back({{slot, choices}, entered});
    }
    if (read_token(input) != "END_REFILL_CURSOR") {
        throw std::runtime_error("missing serialized refill cursor end marker");
    }
    return cursor;
}

}  // namespace m3
