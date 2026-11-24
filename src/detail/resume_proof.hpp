#pragma once

#include "verify.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <istream>
#include <limits>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace m3 {

struct RootWitness {
    Swap action;
    std::vector<std::vector<std::uint8_t>> refills;
    Certificate continuation;
};

struct FailedStateProof {
    State state;
    std::optional<Swap> losing_action;
};

struct RootFailureProof {
    Swap action;
    std::vector<FailedStateProof> states;
};

struct ProofBank {
    State initial;
    Rules rules;
    std::vector<RootWitness> wins;
    std::vector<RootFailureProof> fails;
};

namespace resume_proof_detail {

inline bool playable(const State& initial, const Rules& rules)
{
    return initial.goal > 0 && initial.moves > 0 && rules.refill_cap >= 0 &&
           rules.cascade_budget >= 0 && verification::stable_board(initial.board);
}

inline bool legal(const Board& board, Swap action)
{
    const auto actions = verification::swaps(board);
    return std::ranges::find(actions, action) != actions.end();
}

struct StateHash {
    std::size_t operator()(const State& state) const
    {
        const auto& board = state.board;
        const auto cells = std::string_view(reinterpret_cast<const char*>(board.cells.data()),
                                            board.cells.size());
        auto hash = std::hash<std::string_view>{}(cells);
        for (const int value : {board.rows, board.cols, board.colors, state.goal, state.moves}) {
            hash ^= static_cast<std::size_t>(value) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
        }
        return hash;
    }
};

class FailureVerifier {
    struct Entry {
        std::optional<Swap> action;
        std::optional<bool> verified;
    };

public:
    explicit FailureVerifier(const Rules& rules) : rules_(rules) {}

    bool verify(const State& initial, const RootFailureProof& proof)
    {
        if (!playable(initial, rules_) || !legal(initial.board, proof.action)) {
            return false;
        }
        for (const auto& record : proof.states) {
            const auto& state = record.state;
            const auto& board = state.board;
            if (board.rows != initial.board.rows || board.cols != initial.board.cols ||
                board.colors != initial.board.colors || state.goal <= 0 || state.goal > initial.goal ||
                state.moves < 0 || state.moves > initial.moves || !verification::stable_board(board) ||
                (state.moves == 0 && record.losing_action) ||
                !records_.emplace(state, Entry{record.losing_action, {}}).second) {
                return false;
            }
        }
        return after_swap(initial, proof.action);
    }

private:
    bool after_swap(const State& state, Swap action)
    {
        Board board = state.board;
        std::swap(board.cells[action.a], board.cells[action.b]);
        return prove_turn(board, state.goal, state.moves - 1, 0);
    }

    bool prove_stable(const State& state)
    {
        if (state.goal == 0) {
            return false;
        }
        if (state.moves == 0) {
            return true;
        }
        const auto record = records_.find(state);
        if (record != records_.end() && record->second.verified) {
            return *record->second.verified;
        }

        const auto actions = verification::swaps(state.board);
        if (record == records_.end()) {
            return actions.empty();
        }
        auto& entry = record->second;
        entry.verified = entry.action ?
            std::ranges::find(actions, *entry.action) != actions.end() && after_swap(state, *entry.action) :
            actions.empty();
        return *entry.verified;
    }

    bool prove_turn(const Board& board, int goal, int moves, int wave)
    {
        const auto matches = verification::matches(board);
        if (matches.empty()) {
            return prove_stable({board, goal, moves});
        }
        if (matches.size() > static_cast<std::size_t>(rules_.refill_cap)) {
            return true;
        }
        if (wave >= rules_.cascade_budget) {
            return false;
        }

        const auto falling = verification::drop(board, matches);
        const int cleared = static_cast<int>(matches.size());
        const int remaining = std::max(0, goal - cleared);
        std::vector<std::uint8_t> batch;
        for (int color = 0; color < board.colors; ++color) {
            const int count = cleared / board.colors + (color < cleared % board.colors);
            batch.insert(batch.end(), count, static_cast<std::uint8_t>(color));
        }

        // Starting with the sorted multiset visits each balanced refill once.
        Board filled = falling.board;
        do {
            verification::apply_batch(filled, falling.holes, batch);
            if (!prove_turn(filled, remaining, moves, wave + 1)) {
                return false;
            }
        } while (std::next_permutation(batch.begin(), batch.end()));
        return true;
    }

    Rules rules_;
    std::unordered_map<State, Entry, StateHash> records_;
};

inline void write_state(std::ostream& output, const State& state)
{
    const auto& board = state.board;
    output << board.rows << ' ' << board.cols << ' ' << board.colors << ' '
           << state.goal << ' ' << state.moves;
    for (const auto color : board.cells) {
        output << ' ' << static_cast<int>(color);
    }
    output << '\n';
}

inline void write_batches(std::ostream& output, const std::vector<std::vector<std::uint8_t>>& batches)
{
    output << batches.size() << '\n';
    for (const auto& batch : batches) {
        output << batch.size();
        for (const auto color : batch) {
            output << ' ' << static_cast<int>(color);
        }
        output << '\n';
    }
}

class Reader {
public:
    explicit Reader(std::istream& input) : input_(input) {}

    void token(const char* expected)
    {
        std::string value;
        verification::require(static_cast<bool>(input_ >> value) && value == expected,
                              "invalid proof bank token");
    }

    int integer()
    {
        int value = 0;
        verification::require(static_cast<bool>(input_ >> value), "invalid proof bank integer");
        return value;
    }

    std::size_t count()
    {
        const int value = integer();
        verification::require(value >= 0, "negative proof bank count");
        return static_cast<std::size_t>(value);
    }

    Swap action()
    {
        return {integer(), integer()};
    }

    State state(const Board* shape = nullptr, std::size_t* edge_count = nullptr)
    {
        const int rows = integer();
        const int cols = integer();
        const int colors = integer();
        const int goal = integer();
        const int moves = integer();
        verification::require(rows > 0 && cols > 0 && rows <= std::numeric_limits<int>::max() / cols &&
                              colors > 0 && colors <= empty && goal >= 0 && moves >= 0,
                              "invalid proof bank state");
        verification::require(!shape || (rows == shape->rows && cols == shape->cols && colors == shape->colors),
                              "proof bank state uses different board dimensions or colors");
        if (edge_count) {
            *edge_count = count();
        }
        std::vector<std::uint8_t> cells;
        for (int index = 0; index < rows * cols; ++index) {
            cells.push_back(color(colors));
        }
        Board board(rows, cols, colors);
        board.cells = std::move(cells);
        return {std::move(board), goal, moves};
    }

    std::vector<std::vector<std::uint8_t>> batches(int colors)
    {
        std::vector<std::vector<std::uint8_t>> values;
        const auto waves = count();
        for (std::size_t wave = 0; wave < waves; ++wave) {
            auto& batch = values.emplace_back();
            const auto size = count();
            for (std::size_t index = 0; index < size; ++index) {
                batch.push_back(color(colors));
            }
        }
        return values;
    }

    Certificate certificate(const Rules& rules, const Board& shape)
    {
        std::string encoded;
        verification::require(static_cast<bool>(input_ >> std::quoted(encoded)),
                              "invalid proof bank continuation");
        std::istringstream certificate_input(encoded);
        Reader nested(certificate_input);
        nested.token("M3CERT");
        verification::require(nested.integer() == 1, "unsupported proof bank continuation version");
        const Rules certificate_rules{nested.integer(), nested.integer()};
        verification::require(certificate_rules.refill_cap == rules.refill_cap &&
                              certificate_rules.cascade_budget == rules.cascade_budget,
                              "proof bank continuation rules differ");
        Certificate cert{{}, nested.count()};
        const auto nodes = nested.count();
        for (std::size_t index = 0; index < nodes; ++index) {
            std::size_t edges = 0;
            auto& node = cert.nodes.emplace_back(CertificateNode{nested.state(&shape, &edges), {}});
            for (std::size_t edge = 0; edge < edges; ++edge) {
                const Swap action = nested.action();
                const auto child = nested.count();
                node.edges.push_back({action, nested.batches(shape.colors), child});
            }
        }
        nested.end();
        return cert;
    }

    void end()
    {
        input_ >> std::ws;
        verification::require(input_.eof() && !input_.bad(), "unexpected proof bank trailing data");
    }

private:
    std::uint8_t color(int colors)
    {
        const int value = integer();
        verification::require(value >= 0 && value < colors, "invalid proof bank color");
        return static_cast<std::uint8_t>(value);
    }

    std::istream& input_;
};

} // namespace resume_proof_detail

inline bool verify_root_witness(const State& initial, const Rules& rules, const RootWitness& witness)
{
    if (!resume_proof_detail::playable(initial, rules) ||
        witness.continuation.root >= witness.continuation.nodes.size() ||
        !resume_proof_detail::legal(initial.board, witness.action)) {
        return false;
    }
    const auto& child = witness.continuation.nodes[witness.continuation.root].state;
    const Edge prefix{witness.action, witness.refills, witness.continuation.root};
    return verification::replay_edge(initial, rules, prefix, child) &&
           verify_certificate(child, rules, witness.continuation);
}

inline bool verify_root_failure(const State& initial, const Rules& rules, const RootFailureProof& proof)
{
    return resume_proof_detail::FailureVerifier(rules).verify(initial, proof);
}

inline void write_proof_bank(std::ostream& output, const ProofBank& bank)
{
    using namespace resume_proof_detail;
    output << "M3PROOF 1\n" << bank.rules.refill_cap << ' ' << bank.rules.cascade_budget << '\n';
    write_state(output, bank.initial);
    output << bank.wins.size() << ' ' << bank.fails.size() << '\n';
    for (const auto& win : bank.wins) {
        output << "WIN " << win.action.a << ' ' << win.action.b << '\n';
        write_batches(output, win.refills);
        std::ostringstream continuation;
        write_certificate(continuation, bank.rules, win.continuation);
        output << std::quoted(continuation.str()) << '\n';
    }
    for (const auto& fail : bank.fails) {
        output << "FAIL " << fail.action.a << ' ' << fail.action.b << ' ' << fail.states.size() << '\n';
        for (const auto& record : fail.states) {
            write_state(output, record.state);
            if (record.losing_action) {
                output << 1 << ' ' << record.losing_action->a << ' ' << record.losing_action->b << '\n';
            } else {
                output << 0 << '\n';
            }
        }
    }
    verification::require(static_cast<bool>(output), "proof bank write failed");
}

inline ProofBank read_proof_bank(std::istream& input)
{
    resume_proof_detail::Reader reader(input);
    reader.token("M3PROOF");
    verification::require(reader.integer() == 1, "unsupported proof bank version");
    const Rules rules{reader.integer(), reader.integer()};
    verification::require(rules.refill_cap >= 0 && rules.cascade_budget >= 0,
                          "invalid proof bank rules");
    ProofBank bank{reader.state(), rules, {}, {}};
    const auto wins = reader.count();
    const auto fails = reader.count();
    for (std::size_t index = 0; index < wins; ++index) {
        reader.token("WIN");
        const auto move = reader.action();
        auto refills = reader.batches(bank.initial.board.colors);
        bank.wins.push_back({move, std::move(refills), reader.certificate(rules, bank.initial.board)});
    }
    for (std::size_t index = 0; index < fails; ++index) {
        reader.token("FAIL");
        auto& fail = bank.fails.emplace_back(RootFailureProof{reader.action(), {}});
        const auto records = reader.count();
        for (std::size_t record = 0; record < records; ++record) {
            auto state = reader.state(&bank.initial.board);
            const int has_action = reader.integer();
            verification::require(has_action == 0 || has_action == 1, "invalid proof bank action flag");
            const auto action = has_action == 1 ? std::optional{reader.action()} : std::nullopt;
            fail.states.push_back({std::move(state), action});
        }
    }
    reader.end();
    return bank;
}

} // namespace m3
