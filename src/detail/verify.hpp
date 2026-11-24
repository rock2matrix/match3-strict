#pragma once

#include "board.hpp"
#include "solver.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace m3::verification {

inline void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error("verification: " + message);
    }
}

inline Matches matches(const Board& board)
{
    std::vector<bool> marked(board.cells.size());
    for (int row = 0; row < board.rows; ++row) {
        for (int column = 0; column < board.cols; ++column) {
            const int first = row * board.cols + column;
            const auto color = board.cells[first];
            if (color == empty) {
                continue;
            }
            if (column + 2 < board.cols && board.cells[first + 1] == color &&
                board.cells[first + 2] == color) {
                marked[first] = marked[first + 1] = marked[first + 2] = true;
            }
            if (row + 2 < board.rows && board.cells[first + board.cols] == color &&
                board.cells[first + 2 * board.cols] == color) {
                marked[first] = marked[first + board.cols] = marked[first + 2 * board.cols] = true;
            }
        }
    }
    Matches result;
    for (std::size_t cell = 0; cell < marked.size(); ++cell) {
        if (marked[cell]) {
            result.push_back(static_cast<int>(cell));
        }
    }
    return result;
}

inline std::vector<Swap> swaps(const Board& board)
{
    std::vector<Swap> result;
    const auto probe = [&](int first, int second) {
        if (board.cells[first] == board.cells[second]) {
            return;
        }
        Board exchanged = board;
        std::swap(exchanged.cells[first], exchanged.cells[second]);
        if (!matches(exchanged).empty()) {
            result.push_back({first, second});
        }
    };
    const int size = static_cast<int>(board.cells.size());
    for (int first = 0; first < size; ++first) {
        if (first % board.cols + 1 < board.cols) {
            probe(first, first + 1);
        }
        if (first < size - board.cols) {
            probe(first, first + board.cols);
        }
    }
    return result;
}

struct ReferenceFall {
    Board board;
    std::vector<int> holes;
};

inline ReferenceFall drop(const Board& board, const Matches& matched)
{
    Board result(board.rows, board.cols, board.colors);
    std::fill(result.cells.begin(), result.cells.end(), empty);
    std::vector<bool> removed(board.cells.size());
    for (const int index : matched) {
        removed[index] = true;
    }
    std::vector<int> holes;
    for (int column = 0; column < board.cols; ++column) {
        std::vector<std::uint8_t> surviving;
        for (int row = 0; row < board.rows; ++row) {
            const int index = row * board.cols + column;
            if (!removed[index] && board.cells[index] != empty) {
                surviving.push_back(board.cells[index]);
            }
        }
        const int count = board.rows - static_cast<int>(surviving.size());
        for (int row = 0; row < count; ++row) {
            holes.push_back(row * board.cols + column);
        }
        for (int row = count; row < board.rows; ++row) {
            result.cells[row * board.cols + column] = surviving[row - count];
        }
    }
    return {std::move(result), std::move(holes)};
}

inline bool same_swaps(std::vector<Swap> first, std::vector<Swap> second)
{
    const auto less = [](const Swap& left, const Swap& right) {
        return left.a < right.a || (left.a == right.a && left.b < right.b);
    };
    std::sort(first.begin(), first.end(), less);
    std::sort(second.begin(), second.end(), less);
    return first == second;
}

inline void compare_board_engines(std::uint64_t seed, int rows, int cols, int colors)
{
    const Board board = random_stable(rows, cols, colors, seed);
    require(board == random_stable(rows, cols, colors, seed), "seeded boards differ");
    require(matches(board).empty(), "seeded initial board has a match");
    const auto reference_moves = swaps(board);
    require(same_swaps(reference_moves, legal_swaps(board)), "local legal swap set differs");
    require(same_swaps(reference_moves, reference_swaps(board)), "full legal swap set differs");
    int probe = 0;
    for (const auto move : reference_moves) {
        auto new_board = board;
        std::swap(new_board.cells[move.a], new_board.cells[move.b]);
        auto expected_matches = matches(new_board);
        require(expected_matches == full_matches(new_board), "post-swap full scan differs");
        require(expected_matches == dirty_matches(new_board, {move.a, move.b}),
                "post-swap incremental scan differs");
        for (int wave = 0; wave < 4 && !expected_matches.empty(); ++wave) {
            auto expected = drop(new_board, expected_matches);
            auto actual = clear_and_drop(new_board, expected_matches);
            require(expected.board == actual.board && expected.holes == actual.holes,
                    "clear and gravity output differs");
            require(expected.holes.size() == expected_matches.size(), "clear count differs");
            require(expected.holes.size() == static_cast<std::size_t>(
                        std::count(actual.board.cells.begin(), actual.board.cells.end(), empty)),
                    "hole count differs");
            for (const int index : expected.holes) {
                actual.board.cells[index] = static_cast<std::uint8_t>(probe++ % colors);
            }
            new_board = std::move(actual.board);
            expected_matches = matches(new_board);
            require(expected_matches == full_matches(new_board), "post-refill full scan differs");
            require(expected_matches == dirty_matches(new_board, actual.changed),
                    "post-refill incremental scan differs");
        }
    }
}

inline bool quota_valid(const std::vector<std::uint8_t>& batch, int colors)
{
    std::vector<int> counts(colors);
    for (const auto color : batch) {
        if (color >= colors) {
            return false;
        }
        ++counts[color];
    }
    const int size = static_cast<int>(batch.size());
    for (int color = 0; color < colors; ++color) {
        if (counts[color] != size / colors + (color < size % colors)) {
            return false;
        }
    }
    return true;
}

template <class Visit>
bool raw_batches(int holes, int colors, Visit visit)
{
    std::vector<std::uint8_t> batch(holes);
    for (;;) {
        if (quota_valid(batch, colors) && !visit(batch)) {
            return false;
        }
        int position = holes - 1;
        for (; position >= 0 && ++batch[position] == colors; --position) {
            batch[position] = 0;
        }
        if (position < 0) {
            return true;
        }
    }
}

inline void apply_batch(Board& board, const std::vector<int>& holes,
                        const std::vector<std::uint8_t>& batch)
{
    for (std::size_t index = 0; index < holes.size(); ++index) {
        board.cells[holes[index]] = batch[index];
    }
}

inline Status oracle_status(const State& state, const Rules& rules);

inline Status oracle_turn(const Board& board, int goal, int moves_after, int waves,
                          const Rules& rules)
{
    const auto matched = matches(board);
    if (matched.empty()) {
        return oracle_status({board, goal, moves_after}, rules);
    }
    if (static_cast<int>(matched.size()) > rules.refill_cap) {
        return Status::fail;
    }
    if (waves >= rules.cascade_budget) {
        return Status::unknown;
    }

    const auto falling = drop(board, matched);
    const int next_goal = std::max(0, goal - static_cast<int>(matched.size()));
    Status result = Status::fail;
    raw_batches(static_cast<int>(falling.holes.size()), board.colors,
                [&](const std::vector<std::uint8_t>& batch) {
        Board filled = falling.board;
        apply_batch(filled, falling.holes, batch);
        const Status next = oracle_turn(filled, next_goal, moves_after, waves + 1, rules);
        if (next == Status::win) {
            result = Status::win;
            return false;
        }
        if (next == Status::unknown) {
            result = Status::unknown;
        }
        return true;
    });
    return result;
}

inline Status oracle_status(const State& state, const Rules& rules)
{
    if (state.goal <= 0) {
        return Status::win;
    }
    if (state.moves <= 0) {
        return Status::fail;
    }
    const auto actions = swaps(state.board);
    if (actions.empty()) {
        return Status::fail;
    }
    Status result = Status::win;
    for (const auto move : actions) {
        Board exchanged = state.board;
        std::swap(exchanged.cells[move.a], exchanged.cells[move.b]);
        const Status next = oracle_turn(exchanged, state.goal, state.moves - 1, 0, rules);
        if (next == Status::fail) {
            return Status::fail;
        }
        if (next == Status::unknown) {
            result = Status::unknown;
        }
    }
    return result;
}

inline Status oracle_action_status(const State& state, const Rules& rules, Swap move)
{
    const auto actions = swaps(state.board);
    require(state.goal > 0 && state.moves > 0 &&
                std::find(actions.begin(), actions.end(), move) != actions.end(),
            "oracle action is not legal");
    Board exchanged = state.board;
    std::swap(exchanged.cells[move.a], exchanged.cells[move.b]);
    return oracle_turn(exchanged, state.goal, state.moves - 1, 0, rules);
}

struct BatchAudit {
    std::vector<std::uint8_t> batch;
    Status status;
};

inline std::vector<BatchAudit> oracle_first_batches(const State& state, const Rules& rules, Swap move)
{
    const auto actions = swaps(state.board);
    require(state.goal > 0 && state.moves > 0 &&
                std::find(actions.begin(), actions.end(), move) != actions.end(),
            "oracle action is not legal");
    Board exchanged = state.board;
    std::swap(exchanged.cells[move.a], exchanged.cells[move.b]);
    const auto matched = matches(exchanged);
    if (static_cast<int>(matched.size()) > rules.refill_cap || rules.cascade_budget <= 0) {
        return {};
    }
    const auto falling = drop(exchanged, matched);
    const int next_goal = std::max(0, state.goal - static_cast<int>(matched.size()));
    std::vector<BatchAudit> audit;
    raw_batches(static_cast<int>(falling.holes.size()), exchanged.colors,
                [&](const std::vector<std::uint8_t>& batch) {
        Board filled = falling.board;
        apply_batch(filled, falling.holes, batch);
        audit.push_back({batch, oracle_turn(filled, next_goal, state.moves - 1, 1, rules)});
        return true;
    });
    return audit;
}

inline bool stable_board(const Board& board)
{
    return board.rows > 0 && board.cols > 0 && board.colors > 0 && board.colors <= empty &&
        board.cells.size() == static_cast<std::size_t>(board.rows) * board.cols &&
        std::all_of(board.cells.begin(), board.cells.end(), [&](std::uint8_t cell) {
        return cell < board.colors;
    }) && matches(board).empty();
}

inline bool replay_edge(const State& parent, const Rules& rules, const Edge& edge,
                        const State& child)
{
    Board board = parent.board;
    std::swap(board.cells[edge.swap.a], board.cells[edge.swap.b]);
    int goal = parent.goal;
    auto matched = matches(board);
    for (const auto& batch : edge.refills) {
        if (matched.empty() || matched.size() > static_cast<std::size_t>(rules.refill_cap) ||
            batch.size() != matched.size() || !quota_valid(batch, board.colors)) {
            return false;
        }
        goal = std::max(0, goal - static_cast<int>(matched.size()));
        auto falling = drop(board, matched);
        apply_batch(falling.board, falling.holes, batch);
        board = std::move(falling.board);
        matched = matches(board);
    }
    return matched.empty() && !edge.refills.empty() &&
           edge.refills.size() <= static_cast<std::size_t>(rules.cascade_budget) &&
           child == State{std::move(board), goal, parent.moves - 1};
}

} // namespace m3::verification

namespace m3 {

inline bool verify_certificate(const State& root, const Rules& rules, const Certificate& cert)
{
    if (cert.root >= cert.nodes.size() || cert.nodes[cert.root].state != root ||
        rules.refill_cap < 0 || rules.cascade_budget < 0 ||
        root.goal < 0 || root.moves < 0 ||
        !verification::stable_board(root.board)) {
        return false;
    }
    std::vector<bool> verified(cert.nodes.size());
    const auto visit = [&](auto&& self, std::size_t index) -> bool {
        if (verified[index]) {
            return true;
        }
        const auto& node = cert.nodes[index];
        if (node.state.goal <= 0) {
            verified[index] = node.edges.empty();
            return verified[index];
        }
        if (node.state.moves <= 0) {
            return false;
        }
        const auto expected = verification::swaps(node.state.board);
        std::vector<Swap> covered;
        for (const auto& edge : node.edges) {
            covered.push_back(edge.swap);
        }
        if (expected.empty() || !verification::same_swaps(expected, covered)) {
            return false;
        }
        for (const auto& edge : node.edges) {
            if (edge.child >= cert.nodes.size() ||
                !verification::replay_edge(node.state, rules, edge, cert.nodes[edge.child].state) ||
                !self(self, edge.child)) {
                return false;
            }
        }
        verified[index] = true;
        return true;
    };
    return visit(visit, cert.root) && std::all_of(verified.begin(), verified.end(), [](bool value) {
        return value;
    });
}

inline void write_certificate(std::ostream& output, const Rules& rules, const Certificate& cert)
{
    output << "M3CERT 1\n" << rules.refill_cap << ' ' << rules.cascade_budget << ' '
           << cert.root << ' ' << cert.nodes.size() << '\n';
    for (const auto& node : cert.nodes) {
        const auto& state = node.state;
        output << state.board.rows << ' ' << state.board.cols << ' ' << state.board.colors << ' '
               << state.goal << ' ' << state.moves << ' ' << node.edges.size() << '\n';
        for (const auto cell : state.board.cells) {
            output << static_cast<int>(cell) << ' ';
        }
        output << '\n';
        for (const auto& edge : node.edges) {
            output << edge.swap.a << ' ' << edge.swap.b << ' ' << edge.child << ' '
                   << edge.refills.size() << '\n';
            for (const auto& batch : edge.refills) {
                output << batch.size();
                for (const auto color : batch) {
                    output << ' ' << static_cast<int>(color);
                }
                output << '\n';
            }
        }
    }
    verification::require(static_cast<bool>(output), "certificate write failed");
}

inline std::pair<Rules, Certificate> read_certificate(std::istream& input)
{
    const auto integer = [&] {
        int value = 0;
        verification::require(static_cast<bool>(input >> value), "invalid certificate integer");
        return value;
    };
    const auto count = [&] {
        const int value = integer();
        verification::require(value >= 0, "negative certificate count");
        return static_cast<std::size_t>(value);
    };
    const auto color = [&] {
        const int value = integer();
        verification::require(value >= 0 && value < empty, "invalid certificate color");
        return static_cast<std::uint8_t>(value);
    };
    std::string header;
    verification::require(static_cast<bool>(input >> header) && header == "M3CERT" && integer() == 1,
                          "invalid certificate header");
    const Rules rules{integer(), integer()};
    const auto root = count();
    const auto node_count = count();
    Certificate cert{{}, root};
    for (std::size_t index = 0; index < node_count; ++index) {
        const int rows = integer();
        const int cols = integer();
        const int colors = integer();
        const int goal = integer();
        const int moves = integer();
        const auto edge_count = count();
        auto& node = cert.nodes.emplace_back(CertificateNode{{Board(rows, cols, colors), goal, moves}, {}});
        for (auto& cell : node.state.board.cells) {
            cell = color();
        }
        for (std::size_t e = 0; e < edge_count; ++e) {
            const Swap move{integer(), integer()};
            const auto child = count();
            const auto refill_count = count();
            auto& edge = node.edges.emplace_back(Edge{move, {}, child});
            for (std::size_t r = 0; r < refill_count; ++r) {
                const auto batch_count = count();
                auto& batch = edge.refills.emplace_back();
                for (std::size_t c = 0; c < batch_count; ++c) {
                    batch.push_back(color());
                }
            }
        }
    }
    input >> std::ws;
    verification::require(input.eof(), "unexpected certificate trailing data");
    return {rules, std::move(cert)};
}

inline void run_correctness_checks(std::ostream& output)
{
    using verification::require;

    Board cross(5, 5, 5);
    for (int row = 0; row < cross.rows; ++row) {
        for (int col = 0; col < cross.cols; ++col) {
            const int index = row * cross.cols + col;
            cross.cells[index] = row == 2 || (col == 2 && row < 4) ? 0 : 1 + (row + col) % 4;
        }
    }
    const Matches cross_expected{2, 7, 10, 11, 12, 13, 14, 17};
    require(full_matches(cross) == cross_expected && verification::matches(cross) == cross_expected,
            "overlapping length-four and length-five matches are incorrect");
    require(full_matches(Board(5, 5, 5)).empty(), "empty cells matched");

    Board dead(5, 5, 3);
    for (int row = 0; row < dead.rows; ++row) {
        for (int col = 0; col < dead.cols; ++col) {
            dead.cells[row * dead.cols + col] = (row + col) % 3;
        }
    }
    require(legal_swaps(dead).empty() && verification::swaps(dead).empty(), "dead board has a move");
    const Rules small{4, 2};
    require(Solver(small).solve({dead, 1, 2}).status == Status::fail, "dead board did not fail");
    require(Solver(small).solve({dead, 1, 0}).status == Status::fail, "zero moves did not fail");
    const State already_won{dead, 0, 0};
    const auto terminal = Solver(small).solve(already_won);
    require(terminal.status == Status::win && verify_certificate(already_won, small, terminal.certificate),
            "goal at zero moves was not accepted");

    for (std::uint64_t seed = 1; seed <= 48; ++seed) {
        verification::compare_board_engines(seed, 5, 5, 3);
        verification::compare_board_engines(seed, 8, 8, 5);
        verification::compare_board_engines(seed, 5, 9, 5);
        verification::compare_board_engines(seed, 10, 10, 5);
    }

    int refill_cases = 0;
    for (std::uint64_t seed = 1; seed <= 12; ++seed) {
        const auto board = random_stable(5, 5, 3, seed);
        for (const auto move : verification::swaps(board)) {
            auto exchanged = board;
            std::swap(exchanged.cells[move.a], exchanged.cells[move.b]);
            const auto found = verification::matches(exchanged);
            if (found.size() > 4) {
                continue;
            }
            const auto falling = verification::drop(exchanged, found);
            const auto quotas = balanced_quota(board.colors, static_cast<int>(found.size()));
            using Batches = std::vector<std::vector<std::uint8_t>>;
            Batches any_batches;
            Batches stable_batches;
            verification::raw_batches(static_cast<int>(found.size()), board.colors, [&](const auto& batch) {
                any_batches.push_back(batch);
                auto filled = falling.board;
                verification::apply_batch(filled, falling.holes, batch);
                if (verification::matches(filled).empty()) {
                    stable_batches.push_back(batch);
                }
                return true;
            });
            for (const auto mode : {RefillMode::raw, RefillMode::quota, RefillMode::csp}) {
                for (const bool stable : {false, true}) {
                    m3::RefillStats stats;
                    Batches actual;
                    const auto completion = enumerate_refills(falling.board, falling.holes, quotas, mode,
                        stable, true, stats, 100'000, [&](const auto& batch) {
                            actual.push_back(batch);
                            return Visit::next;
                        });
                    const auto less = [](const auto& left, const auto& right) {
                        return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end());
                    };
                    std::sort(actual.begin(), actual.end(), less);
                    require(completion == Visit::next && actual == (stable ? stable_batches : any_batches),
                            "refill generator omitted, duplicated, or added a candidate");
                }
            }
            ++refill_cases;
        }
    }

    Options baseline;
    baseline.csp = false;
    baseline.memo = false;
    baseline.incremental = false;
    baseline.ranked = false;
    baseline.max_states = baseline.max_candidates = baseline.max_refill_nodes = 10'000'000;
    auto csp_only = baseline;
    csp_only.csp = true;
    auto hybrid = baseline;
    hybrid.csp = hybrid.memo = hybrid.incremental = hybrid.ranked = true;

    int states_checked = 0;
    int wins = 0;
    int fails = 0;
    int unknowns = 0;
    int replayed = 0;
    for (const std::uint64_t seed : {1, 8, 10, 28, 54, 79}) {
        const auto board = random_stable(5, 5, 3, seed);
        for (int moves = 1; moves <= 3; ++moves) {
            for (const int goal : {2 * moves, 3 * moves, 5 * moves}) {
                const State state{board, goal, moves};
                const auto expected = verification::oracle_status(state, small);
                ++states_checked;
                wins += expected == Status::win;
                fails += expected == Status::fail;
                unknowns += expected == Status::unknown;
                for (const auto options : {baseline, csp_only, hybrid}) {
                    const auto actual = Solver(small, options).solve(state);
                    require(actual.status == expected, "AND-OR result differs from exhaustive oracle at seed " +
                            std::to_string(seed) + ", goal " + std::to_string(goal) + ", moves " +
                            std::to_string(moves));
                    if (actual.status == Status::win) {
                        require(verify_certificate(state, small, actual.certificate), "certificate replay failed");
                        ++replayed;
                    }
                }
            }
        }
    }
    require(wins > 0 && fails > 0 && unknowns > 0, "oracle corpus did not exercise all outcomes");

    int checked_action_sets = 0;
    int checked_first_batches = 0;
    for (const std::uint64_t seed : {1, 8, 10, 28, 54, 79}) {
        const auto board = random_stable(5, 5, 3, seed);
        for (int depth = 1; depth <= 3; ++depth) {
            const State state{board, 3 * depth, depth};
            auto solver = Solver(small, hybrid);
            for (const auto move : verification::swaps(board)) {
                const auto expected = verification::oracle_first_batches(state, small, move);
                auto action_result = Status::fail;
                for (const auto& entry : expected) {
                    const auto actual = solver.evaluate_first_batch(state, move, entry.batch);
                    require(actual == entry.status, "first-batch continuation differs at seed " +
                            std::to_string(seed) + ", depth " + std::to_string(depth));
                    if (actual == Status::win || (actual == Status::unknown && action_result == Status::fail)) {
                        action_result = actual;
                    }
                    ++checked_first_batches;
                }
                require(action_result == verification::oracle_action_status(state, small, move),
                        "safe first-batch set gives incorrect legal action result");
                ++checked_action_sets;
            }
        }
    }

    const State mixed{random_stable(5, 5, 3, 1), 2, 1};
    bool winning_action = false;
    bool failing_action = false;
    for (const auto move : verification::swaps(mixed.board)) {
        const auto response = verification::oracle_action_status(mixed, small, move);
        winning_action |= response == Status::win;
        failing_action |= response == Status::fail;
    }
    require(winning_action && failing_action && Solver(small).solve(mixed).status == Status::fail,
            "strict guarantee accepted one winning player action in a mixed state");

    const State winning{random_stable(5, 5, 3, 10), 2, 1};
    const auto good = Solver(small, hybrid).solve(winning);
    require(good.status == Status::win && verify_certificate(winning, small, good.certificate),
            "cascade fixture needs a valid win certificate");
    const Rules no_cascade_budget{4, 0};
    require(Solver(no_cascade_budget).solve(winning).status == Status::unknown,
            "cascade search limit reported a definite result");
    auto no_candidates = hybrid;
    no_candidates.max_candidates = 0;
    require(Solver(small, no_candidates).solve(winning).status == Status::unknown,
            "candidate search limit reported a definite result");
    auto no_refill_nodes = hybrid;
    no_refill_nodes.max_refill_nodes = 0;
    require(Solver(small, no_refill_nodes).solve(winning).status == Status::unknown,
            "refill search limit reported a definite result");
    auto no_states = hybrid;
    no_states.max_states = 0;
    require(Solver(small, no_states).solve(winning).status == Status::unknown,
            "state search limit reported a definite result");

    std::stringstream encoded;
    write_certificate(encoded, small, good.certificate);
    const auto [loaded_rules, loaded_certificate] = read_certificate(encoded);
    require(verify_certificate(winning, loaded_rules, loaded_certificate), "certificate round-trip did not replay");

    const auto reject_corruption = [&](auto corrupt, const std::string& label) {
        auto broken = good.certificate;
        corrupt(broken, broken.nodes[broken.root].edges);
        require(!verify_certificate(winning, small, broken), "accepted corrupted certificate: " + label);
    };
    reject_corruption([](auto&, auto& edges) { edges.pop_back(); }, "missing legal swap");
    reject_corruption([](auto&, auto& edges) { edges.push_back(edges.front()); }, "duplicated legal swap");
    reject_corruption([](auto&, auto& edges) { edges.front().swap = {-1, 0}; }, "illegal swap");
    reject_corruption([](auto&, auto& edges) { edges.front().refills.front().front() = empty; }, "color quota");
    reject_corruption([](auto& cert, auto& edges) { edges.front().child = cert.root; }, "cycle");
    reject_corruption([](auto& cert, auto& edges) { ++cert.nodes[edges.front().child].state.moves; }, "step count");
    reject_corruption([](auto& cert, auto&) { cert.nodes.push_back(cert.nodes.front()); }, "unreachable node");
    reject_corruption([](auto&, auto& edges) {
        auto found = std::find_if(edges.begin(), edges.end(), [](const Edge& edge) { return edge.refills.size() > 1; });
        require(found != edges.end(), "cascade fixture needs a multi-batch witness");
        found->refills.pop_back();
    }, "truncated cascade");

    output << "board_differential_cases=192 refill_set_cases=" << refill_cases
           << " oracle_states=" << states_checked << " oracle_win=" << wins << " oracle_fail=" << fails
           << " oracle_unknown=" << unknowns << " certificates_replayed=" << replayed
           << " oracle_action_sets=" << checked_action_sets << " oracle_first_batches=" << checked_first_batches
           << " corrupted_certificates_rejected=8 budget_cutoff_cases=4\n";
}

} // namespace m3
