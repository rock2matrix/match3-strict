#pragma once

#include <match3_strict/solver.hpp>

#include "detail/verify.hpp"

#include <array>
#include <memory>
#include <random>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace refill_study {

namespace strict = match3_strict;

enum class Outcome {
    safe_terminal,
    safe_known,
    safe_solved,
    unsafe_refill_cap,
    unsafe_no_moves,
    unsafe_dead,
    unsafe_solved,
    unknown_depth,
    unknown_search,
    unknown_work_cap,
    count,
};

inline constexpr std::array<std::string_view, static_cast<std::size_t>(Outcome::count)> outcome_names{
    "safe_terminal", "safe_known", "safe_solved", "unsafe_refill_cap", "unsafe_no_moves",
    "unsafe_dead", "unsafe_solved", "unknown_depth", "unknown_search", "unknown_work_cap",
};

struct ProofRef {
    const m3::Certificate* certificate = nullptr;
    std::size_t index = 0;

    [[nodiscard]] const m3::CertificateNode& node() const
    {
        return certificate->nodes.at(index);
    }
};

struct Step {
    ProofRef continuation;
    int goal = 0;
    int moves = 0;
    int clear = 0;
    int extra = 0;
    int waves = 0;
    std::vector<int> wave_sizes;
};

struct Probe {
    Outcome outcome;
    Step step;
    bool same_witness = false;
    bool cache_hit = false;
    bool solver_called = false;

    [[nodiscard]] bool accepted() const noexcept
    {
        return outcome == Outcome::safe_terminal || outcome == Outcome::safe_known ||
               outcome == Outcome::safe_solved;
    }
};

struct OracleStats {
    std::array<std::uint64_t, outcome_names.size()> outcomes{};
    std::uint64_t proposals = 0;
    std::uint64_t cache_hits = 0;
    std::uint64_t solver_calls = 0;
    std::uint64_t certificates_verified = 0;
    std::uint64_t natural_edges_replayed = 0;
    std::uint64_t same_witness = 0;
    std::uint64_t candidate_checks = 0;
    std::uint64_t refill_nodes = 0;
    std::uint64_t states = 0;
    std::uint64_t candidate_cutoffs = 0;
    std::uint64_t refill_cutoffs = 0;
    std::uint64_t state_cutoffs = 0;
    std::uint64_t cascade_cutoffs = 0;
    std::uint64_t certificate_limits = 0;
    std::uint64_t retained_certificate_nodes = 0;
};

class Oracle {
public:
    // The caller verifies the serialized root certificate against its input problem.
    // Cached certificates and their references last for the complete board study.
    Oracle(const m3::Certificate& independently_verified_root, m3::Rules rules,
           strict::SearchBudget budget, std::size_t max_solver_calls = 0)
        : initial_{&independently_verified_root, independently_verified_root.root},
          rules_(rules), budget_(budget), max_solver_calls_(max_solver_calls)
    {
        m3::verification::require(budget_.cascade_limit == rules_.cascade_budget,
                                  "oracle continuation and root limits differ");
        m3::verification::require(initial_.node().state.goal > 0 && initial_.node().state.moves > 0,
                                  "oracle requires a playable verified root");
        known_root_.reserve(independently_verified_root.nodes.size());
        for (std::size_t index = 0; index < independently_verified_root.nodes.size(); ++index) {
            if (const auto& state = independently_verified_root.nodes[index].state; state.goal > 0) {
                known_root_.try_emplace(key(state), ProofRef{initial_.certificate, index});
            }
        }
    }

    [[nodiscard]] ProofRef root() const noexcept { return initial_; }
    [[nodiscard]] const OracleStats& stats() const noexcept { return stats_; }

    [[nodiscard]] Step witness(ProofRef current, const m3::Edge& edge) const
    {
        const ProofRef next{current.certificate, edge.child};
        return make_step(next.node().state, edge, next);
    }

    [[nodiscard]] Probe probe(ProofRef current, const m3::Edge& fallback_edge, std::uint64_t seed)
    {
        const auto& source = current.node().state;
        auto natural = draw_turn(source, fallback_edge.swap, seed);
        if (natural.stop == Stop::refill_cap) {
            return count({Outcome::unsafe_refill_cap, {}});
        }
        if (natural.stop == Stop::depth) {
            return count({Outcome::unknown_depth, {}});
        }
        const auto& next = natural.state;
        const bool same_witness = natural.edge.refills == fallback_edge.refills;
        if (next.goal == 0) {
            return admit(Outcome::safe_terminal, source, natural, {}, same_witness);
        }
        if (next.moves == 0) {
            return count({Outcome::unsafe_no_moves, {}});
        }
        const ProofRef fallback_next{current.certificate, fallback_edge.child};
        if (next == fallback_next.node().state) {
            return admit(Outcome::safe_known, source, natural, fallback_next, same_witness);
        }
        auto state_key = key(next);
        if (const auto known = known_root_.find(state_key); known != known_root_.end()) {
            return admit(Outcome::safe_known, source, natural, known->second, same_witness);
        }
        if (const auto cached = cache_.find(state_key); cached != cache_.end()) {
            return use_entry(cached->second, source, natural, same_witness, true, false);
        }

        if (m3::legal_swaps(next.board).empty()) {
            cache_.emplace(std::move(state_key), CacheEntry{Outcome::unsafe_dead, {}});
            return count({Outcome::unsafe_dead, {}});
        }
        if (max_solver_calls_ != 0 && stats_.solver_calls >= max_solver_calls_) {
            return count({Outcome::unknown_work_cap, {}});
        }

        const auto entry = certify(next);
        cache_.emplace(std::move(state_key), entry);
        return use_entry(entry, source, natural, same_witness, false, true);
    }

private:
    enum class Stop { stable, refill_cap, depth };

    struct NaturalTurn {
        m3::State state;
        m3::Edge edge;
        Stop stop = Stop::stable;
    };

    struct CacheEntry {
        Outcome outcome;
        ProofRef proof;
    };

    [[nodiscard]] static std::string key(const m3::State& state)
    {
        std::string result(reinterpret_cast<const char*>(state.board.cells.data()), state.board.cells.size());
        result.append(reinterpret_cast<const char*>(&state.goal), sizeof(state.goal));
        result.append(reinterpret_cast<const char*>(&state.moves), sizeof(state.moves));
        return result;
    }

    [[nodiscard]] NaturalTurn draw_turn(const m3::State& source, m3::Swap swap, std::uint64_t seed) const
    {
        std::mt19937_64 random(seed);
        NaturalTurn result{{source.board, source.goal, source.moves - 1}, {swap, {}, 0}};
        auto& state = result.state;
        std::swap(state.board.cells[swap.a], state.board.cells[swap.b]);
        auto matches = m3::dirty_matches(state.board, {swap.a, swap.b});
        m3::verification::require(!matches.empty(), "natural probe got an illegal swap");
        while (!matches.empty()) {
            if (matches.size() > static_cast<std::size_t>(rules_.refill_cap)) {
                result.stop = Stop::refill_cap;
                break;
            }
            if (result.edge.refills.size() >= static_cast<std::size_t>(rules_.cascade_budget)) {
                result.stop = Stop::depth;
                break;
            }
            auto falling = m3::clear_and_drop(state.board, matches);
            const auto holes = static_cast<int>(falling.holes.size());
            const auto quota = m3::balanced_quota(state.board.colors, holes);
            auto& batch = result.edge.refills.emplace_back();
            batch.reserve(falling.holes.size());
            for (int color = 0; color < state.board.colors; ++color) {
                batch.insert(batch.end(), quota[color], static_cast<std::uint8_t>(color));
            }
            for (int index = holes - 1; index > 0; --index) {
                std::swap(batch[index], batch[m3::board_detail::uniform_below(random, index + 1)]);
            }
            m3::verification::apply_batch(falling.board, falling.holes, batch);
            state.goal = std::max(0, state.goal - holes);
            state.board = std::move(falling.board);
            matches = m3::dirty_matches(state.board, falling.changed);
        }
        return result;
    }

    [[nodiscard]] static Step make_step(const m3::State& next, const m3::Edge& edge, ProofRef continuation)
    {
        Step result{next.goal > 0 ? continuation : ProofRef{}, next.goal, next.moves, 0, 0,
                    static_cast<int>(edge.refills.size()), {}};
        result.wave_sizes.reserve(edge.refills.size());
        for (const auto& batch : edge.refills) {
            result.wave_sizes.push_back(static_cast<int>(batch.size()));
            result.clear += static_cast<int>(batch.size());
        }
        result.extra = result.clear - result.wave_sizes.at(0);
        return result;
    }

    [[nodiscard]] CacheEntry certify(const m3::State& state)
    {
        const auto& board = state.board;
        const strict::Problem problem{{board.rows, board.cols, board.colors, board.cells}, state.goal,
                                      state.moves, {rules_.refill_cap}};
        const auto result = strict::solve(problem, budget_);
        const auto& work = result.work;
        stats_.candidate_checks += work.candidate_checks;
        stats_.refill_nodes += work.refill_nodes;
        stats_.states += work.states;
        stats_.candidate_cutoffs += work.candidate_cutoffs;
        stats_.refill_cutoffs += work.refill_cutoffs;
        stats_.state_cutoffs += work.state_cutoffs;
        stats_.cascade_cutoffs += work.cascade_cutoffs;
        stats_.certificate_limits += work.certificate_limit;
        if (result.verdict == strict::Verdict::fail_certified) {
            return {Outcome::unsafe_solved, {}};
        }
        if (result.verdict == strict::Verdict::unknown) {
            return {Outcome::unknown_search, {}};
        }

        m3::verification::require(strict::verify_certificate(problem, result.certificate, rules_.cascade_budget),
                                  "independent verification of newly certified natural successor failed");
        std::istringstream encoded(result.certificate);
        auto [parsed_rules, parsed] = m3::read_certificate(encoded);
        m3::verification::require(parsed_rules.refill_cap == rules_.refill_cap &&
                                      parsed_rules.cascade_budget == rules_.cascade_budget &&
                                      parsed.nodes.at(parsed.root).state == state,
                                  "new continuation disagrees with the verified proposal endpoint");
        ++stats_.certificates_verified;
        stats_.retained_certificate_nodes += parsed.nodes.size();
        const auto& retained = proofs_.emplace_back(std::make_unique<const m3::Certificate>(std::move(parsed)));
        return {Outcome::safe_solved, {retained.get(), retained->root}};
    }

    [[nodiscard]] Probe admit(Outcome outcome, const m3::State& source, const NaturalTurn& natural,
                              ProofRef continuation, bool same_witness, bool cache_hit = false,
                              bool solver_called = false)
    {
        const auto& next = natural.state;
        m3::verification::require(m3::verification::replay_edge(source, rules_, natural.edge, next),
                                  "independent replay of accepted natural turn failed");
        m3::verification::require(next.goal == 0 ||
                                      (continuation.certificate != nullptr && continuation.node().state == next),
                                  "accepted playable proposal lacks an exact verified continuation");
        ++stats_.natural_edges_replayed;
        return count({outcome, make_step(next, natural.edge, continuation), same_witness, cache_hit, solver_called});
    }

    [[nodiscard]] Probe use_entry(CacheEntry entry, const m3::State& source, const NaturalTurn& natural,
                                  bool same_witness, bool cache_hit, bool solver_called)
    {
        if (entry.outcome == Outcome::safe_solved) {
            return admit(entry.outcome, source, natural, entry.proof, same_witness, cache_hit, solver_called);
        }
        return count({entry.outcome, {}, false, cache_hit, solver_called});
    }

    [[nodiscard]] Probe count(Probe probe)
    {
        ++stats_.proposals;
        ++stats_.outcomes[static_cast<std::size_t>(probe.outcome)];
        stats_.same_witness += probe.same_witness;
        stats_.cache_hits += probe.cache_hit;
        stats_.solver_calls += probe.solver_called;
        return probe;
    }

    ProofRef initial_;
    m3::Rules rules_;
    strict::SearchBudget budget_;
    std::size_t max_solver_calls_;
    std::unordered_map<std::string, ProofRef> known_root_;
    std::unordered_map<std::string, CacheEntry> cache_;
    std::vector<std::unique_ptr<const m3::Certificate>> proofs_;
    OracleStats stats_;
};

} // namespace refill_study
