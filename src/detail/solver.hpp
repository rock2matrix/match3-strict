#pragma once

#include "board.hpp"
#include "refill.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace m3 {

struct State {
    Board board;
    int goal;
    int moves;
    bool operator==(const State&) const = default;
};

enum class Status { win, fail, unknown };

inline const char* status_name(Status status) {
    switch (status) {
        case Status::win: return "WIN_CERTIFIED";
        case Status::fail: return "FAIL_CERTIFIED";
        case Status::unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

struct Rules {
    int refill_cap = 12;
    int cascade_budget = 3;
};

struct Edge {
    Swap swap;
    std::vector<std::vector<uint8_t>> refills;
    size_t child;
};

struct CertificateNode {
    State state;
    std::vector<Edge> edges;
};

struct Certificate {
    std::vector<CertificateNode> nodes;
    size_t root = 0;
};

struct Options {
    bool csp = true;
    bool memo = true;
    bool incremental = true;
    bool ranked = true;
    uint64_t stable_prefix = 8;
    uint64_t beam_width = 0;
    uint64_t max_states = 6000;
    uint64_t max_candidates = 8000;
    uint64_t max_refill_nodes = 180000;
    bool fair_root = false;
    bool successor_dedup = false;
};

struct Metrics {
    uint64_t calls{}, states{}, memo_lookups{}, memo_hits{}, memo_entries{}, candidates{};
    uint64_t refill_states{}, cascade_cutoffs{}, complexity_cutoffs{}, beam_cutoffs{};
    uint64_t candidate_cutoffs{}, refill_entry_cutoffs{}, state_cutoffs{};
    uint64_t over_cap{}, depth_sum{}, max_depth{}, max_tested_batch{}, max_clear_waves{};
    uint64_t successor_visits{}, successor_classes{}, successor_reuses{};
    RefillStats refill;
};

struct FirstSwapStats {
    Swap swap{};
    std::optional<Status> status{};
    uint64_t candidates{}, refill_nodes{}, states{};
    uint64_t candidate_cutoffs{}, refill_cutoffs{}, state_cutoffs{}, cascade_cutoffs{}, beam_cutoffs{};
    bool candidate_exhausted_on_entry{}, refill_exhausted_on_entry{}, state_exhausted_on_entry{};
};

struct RootAttemptStats {
    size_t search_order;
    unsigned round;
    uint64_t candidates_before, candidate_target, candidate_allowance;
    FirstSwapStats work;
};

struct SolveResult {
    Status status;
    Certificate certificate;
    Metrics stats;
};

class Solver {
    static constexpr size_t no_node = std::numeric_limits<size_t>::max();

    struct Answer {
        Status status;
        size_t node = no_node;
    };

    struct Move {
        Swap swap;
        Board board;
        Matches hits;
    };

    struct Key {
        std::string cells;
        int goal, moves;
        bool operator==(const Key&) const = default;
    };

    struct KeyHash {
        size_t operator()(const Key& key) const {
            return std::hash<std::string>{}(key.cells) ^
                   (static_cast<size_t>(key.goal) * 0x9e3779b9u) ^
                   (static_cast<size_t>(key.moves) * 0x85ebca6bu);
        }
    };

public:
    Solver(Rules rules = {}, Options options = {}) : rules_(rules), options_(options) {}

    SolveResult solve(const State& initial, std::vector<FirstSwapStats>* first_swaps = nullptr,
                      std::vector<RootAttemptStats>* root_attempts = nullptr) {
        reset(initial.moves);
        if (first_swaps) first_swaps->clear();
        if (root_attempts) root_attempts->clear();
        const auto answer = certify(initial, first_swaps, options_.fair_root, root_attempts);
        metrics_.memo_entries = memo_.size();
        return {answer.status, answer.status == Status::win ? compact(answer.node) : Certificate{}, metrics_};
    }

    Status evaluate_first_batch(const State& state, Swap swap, const std::vector<uint8_t>& values) {
        reset(state.moves);
        const auto swaps = options_.incremental ? legal_swaps(state.board) : reference_swaps(state.board);
        if (state.moves <= 0 || std::ranges::find(swaps, swap) == swaps.end()) return Status::fail;
        auto board = state.board;
        std::swap(board.cells[swap.a], board.cells[swap.b]);
        const auto hits = match_after_edit(board, {swap.a, swap.b});
        const auto count = static_cast<int>(hits.size());
        if (count > rules_.refill_cap || hits.size() != values.size()) return Status::fail;
        if (rules_.cascade_budget == 0) return Status::unknown;
        auto quota = balanced_quota(board.colors, count);
        auto falling = clear_and_drop(board, hits);
        for (size_t i = 0; i < values.size(); ++i) {
            if (values[i] >= quota.size() || --quota[values[i]] < 0) return Status::fail;
            falling.board.cells[falling.holes[i]] = values[i];
        }
        std::vector<std::vector<uint8_t>> witness;
        const auto next_hits = match_after_edit(falling.board, falling.changed);
        return resolve(falling.board, next_hits, std::max(0, state.goal - count), state.moves - 1,
                       1, witness).status;
    }

private:
    void reset(int moves) {
        metrics_ = {};
        certificate_ = {};
        memo_.clear();
        initial_moves_ = moves;
        candidate_limit_ = options_.max_candidates;
    }

    Matches match_after_edit(const Board& board, const std::vector<int>& changed) const {
        return options_.incremental ? dirty_matches(board, changed) : full_matches(board);
    }

    Certificate compact(size_t root) const {
        Certificate result;
        std::vector<size_t> mapping(certificate_.nodes.size(), no_node);
        auto copy = [&](auto&& self, size_t index) -> size_t {
            if (mapping[index] != no_node) return mapping[index];
            auto node = certificate_.nodes[index];
            for (auto& edge : node.edges) edge.child = self(self, edge.child);
            mapping[index] = result.nodes.size();
            result.nodes.push_back(std::move(node));
            return mapping[index];
        };
        result.root = copy(copy, root);
        return result;
    }

    Answer win(const State& state, std::vector<Edge> edges = {}) {
        const auto index = certificate_.nodes.size();
        certificate_.nodes.push_back({state, std::move(edges)});
        return {Status::win, index};
    }

    FirstSwapStats measure_first_swap(Swap swap, Status status, const Metrics& before) const {
        return {
            .swap = swap,
            .status = status,
            .candidates = metrics_.candidates - before.candidates,
            .refill_nodes = metrics_.refill.nodes - before.refill.nodes,
            .states = metrics_.states - before.states,
            .candidate_cutoffs = metrics_.candidate_cutoffs - before.candidate_cutoffs,
            .refill_cutoffs = (metrics_.refill_entry_cutoffs - before.refill_entry_cutoffs)
                + (metrics_.refill.node_cutoffs - before.refill.node_cutoffs),
            .state_cutoffs = metrics_.state_cutoffs - before.state_cutoffs,
            .cascade_cutoffs = metrics_.cascade_cutoffs - before.cascade_cutoffs,
            .beam_cutoffs = metrics_.beam_cutoffs - before.beam_cutoffs,
            .candidate_exhausted_on_entry = before.candidates >= options_.max_candidates,
            .refill_exhausted_on_entry = before.refill.nodes >= options_.max_refill_nodes,
            .state_exhausted_on_entry = before.states >= options_.max_states,
        };
    }

    static void accumulate_swap_work(FirstSwapStats& total, const FirstSwapStats& work) {
        if (!total.status) {
            total = work;
            return;
        }
        total.status = work.status;
        total.candidates += work.candidates;
        total.refill_nodes += work.refill_nodes;
        total.states += work.states;
        total.candidate_cutoffs += work.candidate_cutoffs;
        total.refill_cutoffs += work.refill_cutoffs;
        total.state_cutoffs += work.state_cutoffs;
        total.cascade_cutoffs += work.cascade_cutoffs;
        total.beam_cutoffs += work.beam_cutoffs;
    }

    Answer certify_fair_root(const State& state, const std::vector<Move>& actions,
                            std::vector<FirstSwapStats>* first_swaps,
                            std::vector<RootAttemptStats>* attempts) {
        constexpr std::array<uint64_t, 4> targets{256, 2048, 8192, 32768};
        std::vector<FirstSwapStats> totals;
        totals.reserve(actions.size());
        for (const auto& action : actions) totals.push_back({.swap = action.swap});
        std::vector<Edge> edges;

        auto finish = [&](Answer answer) {
            if (first_swaps) *first_swaps = std::move(totals);
            return answer;
        };
        const auto pending = [](const FirstSwapStats& stat) {
            return !stat.status || stat.status == Status::unknown;
        };
        const auto resources_left = [&] {
            return metrics_.candidates < options_.max_candidates &&
                   metrics_.refill.nodes < options_.max_refill_nodes;
        };
        const auto attempt = [&](size_t index, unsigned round, uint64_t target) {
            const auto& action = actions[index];
            auto& total = totals[index];
            const auto before = metrics_;
            const auto root_spent = total.candidates;
            const auto allowance = std::min(target - root_spent, options_.max_candidates - before.candidates);
            candidate_limit_ = before.candidates + allowance;
            // A retry traverses the action again. All actual work stays in the
            // shared counters, including checks repeated from earlier rounds.
            Edge edge{action.swap, {}, no_node};
            const auto result = resolve(action.board, action.hits, state.goal, state.moves - 1, 0, edge.refills);
            candidate_limit_ = options_.max_candidates;
            const auto work = measure_first_swap(action.swap, result.status, before);
            accumulate_swap_work(total, work);
            if (attempts) attempts->push_back({index + 1, round, root_spent, target, allowance, work});
            if (result.status == Status::win) {
                edge.child = result.node;
                edges.push_back(std::move(edge));
            }
            return result.status;
        };

        for (unsigned round = 0; round < targets.size() && resources_left(); ++round) {
            for (size_t i = 0; i < actions.size() && resources_left(); ++i) {
                if (pending(totals[i]) && attempt(i, round + 1, targets[round]) == Status::fail) {
                    return finish({Status::fail});
                }
            }
            if (edges.size() == actions.size()) return finish(win(state, std::move(edges)));
        }

        // Divide unused capacity over pending actions in the same order.
        // Unused shares of actions that close early go to the remaining actions.
        auto pending_count = actions.size() - edges.size();
        for (size_t i = 0; i < actions.size() && resources_left(); ++i) {
            if (!pending(totals[i])) continue;
            const auto remaining = options_.max_candidates - metrics_.candidates;
            const auto share = remaining / pending_count + (remaining % pending_count != 0);
            if (attempt(i, targets.size() + 1, totals[i].candidates + share) == Status::fail) {
                return finish({Status::fail});
            }
            --pending_count;
        }
        return edges.size() == actions.size()
            ? finish(win(state, std::move(edges))) : finish({Status::unknown});
    }

    Answer certify(const State& state, std::vector<FirstSwapStats>* first_swaps = nullptr,
                   bool fair_root = false, std::vector<RootAttemptStats>* root_attempts = nullptr) {
        ++metrics_.calls;
        if (state.goal == 0) return win(state);
        if (state.moves == 0) return {Status::fail};

        Key key;
        if (options_.memo) {
            ++metrics_.memo_lookups;
            key = {std::string(state.board.cells.begin(), state.board.cells.end()),
                   state.goal, state.moves};
            if (const auto entry = memo_.find(key); entry != memo_.end()) {
                ++metrics_.memo_hits;
                return entry->second;
            }
        }
        if (metrics_.states >= options_.max_states) {
            ++metrics_.complexity_cutoffs;
            ++metrics_.state_cutoffs;
            return {Status::unknown};
        }
        ++metrics_.states;
        const auto depth = static_cast<uint64_t>(initial_moves_ - state.moves);
        metrics_.depth_sum += depth;
        metrics_.max_depth = std::max(metrics_.max_depth, depth);
        auto remember = [&](Answer answer) {
            if (options_.memo && answer.status != Status::unknown) {
                memo_.emplace(std::move(key), answer);
            }
            return answer;
        };

        std::vector<Move> actions;
        const auto swaps = options_.incremental ? legal_swaps(state.board) : reference_swaps(state.board);
        for (const auto swap : swaps) {
            Board board = state.board;
            std::swap(board.cells[swap.a], board.cells[swap.b]);
            auto hits = match_after_edit(board, {swap.a, swap.b});
            actions.push_back({swap, std::move(board), std::move(hits)});
        }
        if (actions.empty()) return remember({Status::fail});
        if (options_.ranked) {
            std::stable_sort(actions.begin(), actions.end(), [&](const Move& a, const Move& b) {
                const auto priority = [&](const Move& move) {
                    const auto count = static_cast<int>(move.hits.size());
                    return count > rules_.refill_cap ? 0 : count;
                };
                return priority(a) < priority(b);
            });
        }

        if (fair_root) return remember(certify_fair_root(state, actions, first_swaps, root_attempts));

        if (first_swaps) {
            first_swaps->reserve(actions.size());
            for (const auto& action : actions) first_swaps->push_back({.swap = action.swap});
        }
        bool unknown = false;
        std::vector<Edge> edges;
        for (size_t i = 0; i < actions.size(); ++i) {
            const auto& action = actions[i];
            Edge edge{action.swap, {}, no_node};
            std::optional<Metrics> before;
            if (first_swaps) before.emplace(metrics_);
            const auto answer = resolve(action.board, action.hits, state.goal, state.moves - 1, 0, edge.refills);
            if (before) (*first_swaps)[i] = measure_first_swap(action.swap, answer.status, *before);
            if (answer.status == Status::fail) return remember(answer);
            if (answer.status == Status::unknown) {
                unknown = true;
            } else {
                edge.child = answer.node;
                edges.push_back(std::move(edge));
            }
        }
        return unknown ? Answer{Status::unknown} : remember(win(state, std::move(edges)));
    }

    Answer resolve(const Board& board, const Matches& matches, int goal, int moves,
                   int wave, std::vector<std::vector<uint8_t>>& witness) {
        if (matches.empty()) return certify({board, goal, moves});
        const auto count = static_cast<int>(matches.size());
        if (count > rules_.refill_cap) {
            ++metrics_.over_cap;
            return {Status::fail};
        }
        if (wave >= rules_.cascade_budget) {
            ++metrics_.cascade_cutoffs;
            return {Status::unknown};
        }
        const bool candidates_exhausted = metrics_.candidates >= candidate_limit_;
        const bool refill_exhausted = metrics_.refill.nodes >= options_.max_refill_nodes;
        if (candidates_exhausted || refill_exhausted) {
            ++metrics_.complexity_cutoffs;
            metrics_.candidate_cutoffs += candidates_exhausted;
            metrics_.refill_entry_cutoffs += refill_exhausted;
            return {Status::unknown};
        }
        ++metrics_.refill_states;
        metrics_.max_clear_waves = std::max(metrics_.max_clear_waves, static_cast<uint64_t>(wave + 1));
        metrics_.max_tested_batch = std::max(metrics_.max_tested_batch, static_cast<uint64_t>(count));
        const int remaining = std::max(0, goal - count);
        const auto falling = clear_and_drop(board, matches);
        const auto quota = balanced_quota(board.colors, count);

        Answer success{Status::unknown};
        bool unknown = false;
        uint64_t tried = 0;
        std::unordered_set<std::string> stable_tried;
        auto check = [&](const std::vector<uint8_t>& values, bool stable) {
            if (options_.beam_width != 0 && tried >= options_.beam_width) {
                ++metrics_.beam_cutoffs;
                return Visit::limit;
            }
            if (metrics_.candidates >= candidate_limit_) {
                ++metrics_.complexity_cutoffs;
                ++metrics_.candidate_cutoffs;
                return Visit::limit;
            }
            ++tried;
            ++metrics_.candidates;
            Board filled = falling.board;
            for (size_t i = 0; i < values.size(); ++i) filled.cells[falling.holes[i]] = values[i];
            Matches next_hits;
            if (!stable) {
                next_hits = match_after_edit(filled, falling.changed);
            }
            witness.push_back(values);
            const auto answer = resolve(filled, next_hits, remaining, moves, wave + 1, witness);
            if (answer.status == Status::win) {
                success = answer;
                return Visit::stop;
            }
            witness.pop_back();
            unknown |= answer.status == Status::unknown;
            return Visit::next;
        };

        if (options_.csp) {
            bool prefix_reached = false;
            const auto result = enumerate_refills(falling.board, falling.holes, quota, RefillMode::csp,
                true, options_.ranked, metrics_.refill, options_.max_refill_nodes,
                [&](const auto& values) {
                    if (options_.stable_prefix != 0 && stable_tried.size() >= options_.stable_prefix) {
                        prefix_reached = true;
                        return Visit::stop;
                    }
                    if (options_.stable_prefix != 0) stable_tried.emplace(values.begin(), values.end());
                    return check(values, true);
                });
            if (result == Visit::limit) return {Status::unknown};
            if (result == Visit::stop && !prefix_reached) return success;
            // A stable board is only a promising ordering tier. If it does not
            // yield a witness, search the remaining legal (including cascade) refills.
            const auto rest = enumerate_refills(falling.board, falling.holes, quota, RefillMode::quota,
                false, options_.ranked, metrics_.refill, options_.max_refill_nodes,
                [&](const auto& values) {
                    if (prefix_reached) {
                        if (stable_tried.contains(std::string(values.begin(), values.end()))) return Visit::next;
                    } else {
                        Board filled = falling.board;
                        for (size_t i = 0; i < values.size(); ++i) filled.cells[falling.holes[i]] = values[i];
                        const auto hits = match_after_edit(filled, falling.changed);
                        if (hits.empty()) return Visit::next;
                    }
                    return check(values, false);
                });
            if (rest == Visit::stop) return success;
            if (rest == Visit::limit) return {Status::unknown};
        } else {
            const auto result = enumerate_refills(falling.board, falling.holes, quota, RefillMode::quota,
                false, options_.ranked, metrics_.refill, options_.max_refill_nodes,
                [&](const auto& values) { return check(values, false); });
            if (result == Visit::stop) return success;
            if (result == Visit::limit) return {Status::unknown};
        }
        return {unknown ? Status::unknown : Status::fail};
    }

    Rules rules_;
    Options options_;
    Metrics metrics_;
    uint64_t candidate_limit_ = 0;
    int initial_moves_ = 0;
    Certificate certificate_;
    std::unordered_map<Key, Answer, KeyHash> memo_;
};

} // namespace m3
