#pragma once

#include "board.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <utility>
#include <vector>

namespace m3 {

enum class RefillMode { raw, quota, csp };
enum class Visit { next, stop, limit };

struct RefillStats {
    std::uint64_t nodes{};
    std::uint64_t node_cutoffs{};
    std::uint64_t leaves{};
    std::uint64_t emitted{};
    std::uint64_t pruned{};
    std::uint64_t quota_rejected_leaves{};
    std::uint64_t unstable_leaves{};
    std::uint64_t infeasible_domains{};
};

inline std::vector<int> balanced_quota(int colors, int holes)
{
    if (colors <= 0 || holes < 0) {
        throw std::invalid_argument("invalid refill dimensions");
    }
    std::vector<int> quota(colors, holes / colors);
    for (int color = 0; color < holes % colors; ++color) {
        ++quota[color];
    }
    return quota;
}

namespace refill_detail {

using Mask = std::uint64_t;
using Triplet = std::array<int, 3>;
using Pair = std::array<int, 2>;

inline constexpr int max_colors = 64;

template<class Callback>
class Enumerator {
public:
    Enumerator(const Board& partial, const std::vector<int>& holes,
               const std::vector<int>& quota, bool stable, bool ranked,
               RefillStats& stats, std::uint64_t node_limit, Callback& callback)
        : board_(partial), holes_(holes), remaining_(quota), stable_(stable),
          ranked_(ranked), stats_(stats), node_limit_(node_limit),
          callback_(callback), values_(holes.size()), contacts_(holes.size()),
          priority_(holes.size())
    {
        if (board_.colors < 1 || board_.colors > max_colors ||
            quota.size() != static_cast<std::size_t>(board_.colors)) {
            throw std::invalid_argument("invalid refill color count");
        }
        if (std::ranges::any_of(quota, [](int count) { return count < 0; }) ||
            std::accumulate(quota.begin(), quota.end(), std::int64_t{0}) !=
                static_cast<std::int64_t>(holes_.size())) {
            throw std::invalid_argument("refill quota must sum to the number of holes");
        }
        std::vector<int> slot_by_cell(board_.cells.size(), -1);
        std::vector<int> holes_per_column(board_.cols);
        for (std::size_t slot = 0; slot < holes_.size(); ++slot) {
            const int cell = holes_[slot];
            if (cell < 0 || static_cast<std::size_t>(cell) >= board_.cells.size() ||
                board_.cells[cell] != empty || slot_by_cell[cell] >= 0) {
                throw std::invalid_argument("invalid refill hole");
            }
            slot_by_cell[cell] = static_cast<int>(slot);
            ++holes_per_column[cell % board_.cols];
        }
        if (std::ranges::count(board_.cells, empty) !=
            static_cast<std::ptrdiff_t>(holes_.size())) {
            throw std::invalid_argument("refill must cover every empty cell");
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
    }

    Visit run(RefillMode mode)
    {
        if (mode == RefillMode::raw) {
            return raw(0);
        }
        if (mode == RefillMode::csp && stable_) {
            return csp(0);
        }
        return quota(0);
    }

private:
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

    bool enter_node()
    {
        if (stats_.nodes >= node_limit_) {
            ++stats_.node_cutoffs;
            return false;
        }
        ++stats_.nodes;
        return true;
    }

    Visit emit()
    {
        ++stats_.emitted;
        return callback_(std::as_const(values_));
    }

    Visit check_stability_and_emit()
    {
        if (stable_ && (fixed_match_ ||
            std::ranges::any_of(windows_, [this](const Triplet& window) {
                return is_match(window);
            }))) {
            ++stats_.pruned;
            ++stats_.unstable_leaves;
            return Visit::next;
        }
        return emit();
    }

    void assign(std::size_t slot, int color)
    {
        --remaining_[color];
        values_[slot] = static_cast<std::uint8_t>(color);
        board_.cells[holes_[slot]] = values_[slot];
    }

    void unassign(std::size_t slot, int color)
    {
        ++remaining_[color];
        board_.cells[holes_[slot]] = empty;
    }

    Visit raw(std::size_t slot)
    {
        if (!enter_node()) {
            return Visit::limit;
        }
        if (slot == holes_.size()) {
            ++stats_.leaves;
            if (std::ranges::any_of(remaining_, [](int count) { return count != 0; })) {
                ++stats_.pruned;
                ++stats_.quota_rejected_leaves;
                return Visit::next;
            }
            return check_stability_and_emit();
        }

        for (int color = 0; color < board_.colors; ++color) {
            assign(slot, color);
            const Visit result = raw(slot + 1);
            unassign(slot, color);
            if (result != Visit::next) {
                return result;
            }
        }
        return Visit::next;
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
        if (!ranked_) {
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

    Visit quota(std::size_t slot)
    {
        if (!enter_node()) {
            return Visit::limit;
        }
        if (slot == holes_.size()) {
            ++stats_.leaves;
            return check_stability_and_emit();
        }

        Mask choices = remaining_colors();
        stats_.pruned += board_.colors - std::popcount(choices);
        while (choices != 0) {
            const int color = best_color(slot, choices);
            choices ^= Mask{1} << color;
            assign(slot, color);
            const Visit result = quota(slot + 1);
            unassign(slot, color);
            if (result != Visit::next) {
                return result;
            }
        }
        return Visit::next;
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

    Visit infeasible()
    {
        ++stats_.pruned;
        ++stats_.infeasible_domains;
        return Visit::next;
    }

    Visit csp(std::size_t depth)
    {
        if (!enter_node()) {
            return Visit::limit;
        }
        if (fixed_match_) {
            return infeasible();
        }
        if (depth == holes_.size()) {
            ++stats_.leaves;
            return emit();
        }

        const Mask available = remaining_colors();
        std::array<int, max_colors> support{};
        std::size_t selected = 0;
        Mask choices = 0;
        int best_size = max_colors + 1;
        for (std::size_t slot = 0; slot < holes_.size(); ++slot) {
            if (board_.cells[holes_[slot]] != empty) {
                continue;
            }
            const Mask legal = legal_colors(slot, available);
            const int size = std::popcount(legal);
            if (size == 0) {
                return infeasible();
            }
            if (size < best_size || (size == best_size && priority_[slot] > priority_[selected])) {
                selected = slot;
                choices = legal;
                best_size = size;
            }
            for (Mask supported = legal; supported != 0; supported &= supported - 1) {
                ++support[std::countr_zero(supported)];
            }
        }
        for (Mask colors = available; colors != 0; colors &= colors - 1) {
            const int color = std::countr_zero(colors);
            if (support[color] < remaining_[color]) {
                return infeasible();
            }
        }

        // If all positions that support a color are needed, each is forced to it.
        // Intersect the selected domain with every such requirement.
        for (Mask colors = choices; colors != 0; colors &= colors - 1) {
            const int color = std::countr_zero(colors);
            if (support[color] == remaining_[color]) {
                choices &= Mask{1} << color;
            }
        }
        if (choices == 0) {
            return infeasible();
        }

        stats_.pruned += board_.colors - std::popcount(choices);
        while (choices != 0) {
            const int color = best_color(selected, choices);
            choices ^= Mask{1} << color;
            assign(selected, color);
            const Visit result = csp(depth + 1);
            unassign(selected, color);
            if (result != Visit::next) {
                return result;
            }
        }
        return Visit::next;
    }

    Board board_;
    const std::vector<int>& holes_;
    std::vector<int> remaining_;
    bool stable_;
    bool ranked_;
    RefillStats& stats_;
    std::uint64_t node_limit_;
    Callback& callback_;
    std::vector<std::uint8_t> values_;
    std::vector<std::vector<Pair>> contacts_;
    std::vector<std::array<int, 2>> priority_;
    std::vector<Triplet> windows_;
    bool fixed_match_{};
};

}  // namespace refill_detail

// The callback receives one complete assignment in the caller's hole order.
// Only stable=true excludes batches that immediately create or retain a match.
// A next return means exhaustive completion under the requested filter.
template<class Callback>
Visit enumerate_refills(const Board& partial, const std::vector<int>& holes,
                        const std::vector<int>& quota, RefillMode mode,
                        bool require_stable, bool ranked, RefillStats& stats,
                        std::uint64_t node_limit, Callback&& callback)
{
    refill_detail::Enumerator<Callback> enumerator(partial, holes, quota, require_stable,
                                                   ranked, stats, node_limit, callback);
    return enumerator.run(mode);
}

}  // namespace m3
