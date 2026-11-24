#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace match3_strict {

enum class Verdict {
    win_certified,
    fail_certified,
    unknown,
};

struct Board {
    int rows{};
    int columns{};
    int colors = 5;
    // Row-major color indices in [0, colors).
    std::vector<std::uint8_t> cells;
};

struct Rules {
    // Structural limit per refill batch, including each cascade wave.
    int max_refill = 12;
};

struct Problem {
    Board board;
    int goal{};
    int moves{};
    Rules rules{};
};

struct SearchBudget {
    std::uint64_t max_candidates = 256'000;
    std::uint64_t max_refill_nodes = 2'880'000;
    std::uint64_t max_states = 6'000;
    // Search coverage bound; reaching it does not independently prove a loss.
    int cascade_limit = 9;
};

struct Work {
    std::uint64_t candidate_checks{};
    std::uint64_t refill_nodes{};
    std::uint64_t states{};
    std::uint64_t legal_swaps{};
    std::uint64_t safe_swaps{};
    std::uint64_t candidate_cutoffs{};
    std::uint64_t refill_cutoffs{};
    std::uint64_t state_cutoffs{};
    std::uint64_t cascade_cutoffs{};
    bool certificate_limit{};
};

struct Result {
    Verdict verdict = Verdict::unknown;
    Work work{};
    // M3CERT 1 text for a certified win, empty for the other verdicts.
    std::string certificate;
};

[[nodiscard]] std::string_view verdict_name(Verdict verdict) noexcept;

// Returns a deterministic verdict and counters. Invalid inputs or budgets
// throw std::invalid_argument.
[[nodiscard]] Result solve(const Problem& problem, const SearchBudget& budget = {});

// Independently replays a winning certificate against the exact expected
// board, objective, turns, structural refill rule, and cascade bound.
// Invalid input, incompatible rules, and malformed certificates return false.
[[nodiscard]] bool verify_certificate(const Problem& expected, std::string_view certificate,
                                      int cascade_limit = SearchBudget{}.cascade_limit) noexcept;

} // namespace match3_strict
