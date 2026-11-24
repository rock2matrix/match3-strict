#pragma once

#include "proofbank_proofs.hpp"

#include <algorithm>
#include <array>
#include <iomanip>
#include <numeric>
#include <optional>
#include <ostream>
#include <sstream>
#include <unordered_map>

namespace refill_study {

struct ProofBankExpansionAccess {
    static std::optional<Outcome> lookup_indexed(const Oracle& bank, const std::string& key)
    {
        if (bank.known_root_.contains(key)) {
            return Outcome::safe_known;
        }
        if (const auto entry = bank.cache_.find(key); entry != bank.cache_.end()) {
            return entry->second.outcome;
        }
        return std::nullopt;
    }

    static Outcome prove_missing(Oracle& bank, const m3::State& parent,
                                 const OfflineBankAccess::NaturalTurn& natural,
                                 std::string_view level_id, std::size_t expansion_number,
                                 std::ostream* evidence)
    {
        const auto key = Oracle::key(natural.state);
        require(natural.stop == OfflineBankAccess::Stop::stable && natural.state.goal > 0 &&
                    natural.state.moves > 0 && !lookup_indexed(bank, key),
                "offline expansion requires an unseen stable playable successor");
        const auto entry = bank.certify(natural.state);
        ++bank.stats_.solver_calls;
        if (entry.outcome == Outcome::safe_solved) {
            (void)OfflineBankAccess::replay(bank, parent, natural, entry.proof);
            ++bank.stats_.natural_edges_replayed;
            if (evidence) {
                proofbank_artifact::write_proven(*evidence, level_id, expansion_number, parent,
                                                  natural.edge, bank.rules_, entry.proof);
            }
            bank.cache_.emplace(key, entry);
        }
        return entry.outcome;
    }
};

} // namespace refill_study

namespace {
namespace proofbank {

enum class Variant { Base, Targeted };

inline std::string_view variant_name(Variant variant)
{
    return variant == Variant::Base ? "base" : "targeted";
}

inline constexpr int profile_episodes_per_agent = 24;
inline constexpr int matched_draws_per_action = 12;
inline constexpr std::uint32_t max_extra_solver_calls = 240;

enum class Category : std::size_t { Safe, Unsafe, Unresolved };

inline Category category(Outcome outcome)
{
    if (is_safe(outcome)) {
        return Category::Safe;
    }
    if (is_unresolved(outcome)) {
        return Category::Unresolved;
    }
    return Category::Unsafe;
}

inline void record(acr::SafetyCounts& counts, Category result)
{
    switch (result) {
    case Category::Safe: ++counts.safe; break;
    case Category::Unsafe: ++counts.unsafe; break;
    case Category::Unresolved: ++counts.unresolved; break;
    }
}

struct ActionInfo {
    ProofRef source;
    const m3::Edge* witness{};
    std::string fingerprint;
    std::string column_holes;
    int bucket{};
    int visits{};
    int proposals{};
    int indexed_checks{};
    int indexed_hits{};
    int stable_unresolved{};
    int capped_or_depth{};
    acr::SafetyCounts profile{};
    acr::SafetyCounts before{};
    acr::SafetyCounts after{};
    std::array<Category, matched_draws_per_action> before_draws{};
    std::array<int, 9> transitions{};
};

struct QueryInfo {
    std::size_t action{};
    std::string successor_key;
    std::string successor_fingerprint;
    Bank::NaturalTurn natural;
    int frequency{1};
    bool base_indexed_unknown{};
    int rank{};
    std::string_view disposition{"not_selected"};
    std::size_t proof_number{};
};

struct ExpansionStats {
    std::uint64_t base_solver_calls{}, base_verified{}, base_proof_nodes{};
    std::uint64_t base_cpu_ns{}, profile_cpu_ns{}, audit_cpu_ns{}, expansion_cpu_ns{};
    std::uint64_t proof_added_nodes{}, extra_candidate_checks{}, extra_refill_nodes{}, extra_states{};
    int profile_turns{}, profile_proposals{}, profile_safe{}, profile_unsafe{}, profile_unresolved{};
    int stable_unresolved{}, repeated_hits{}, prioritized_missing{};
    int solver_calls{}, newly_verified{}, certified_fail{}, solver_unresolved{};
};

struct PreparedExpansion {
    std::unique_ptr<PreparedBoard> board;
    Variant variant{};
    std::uint32_t extra_limit{};
    ExpansionStats stats;
    std::vector<ActionInfo> actions;
    std::vector<QueryInfo> queries;
};

inline std::string fingerprint(const std::string& key)
{
    std::ostringstream output;
    output << std::hex << std::setw(16) << std::setfill('0') << hash_state(key);
    return output.str();
}

inline std::string column_holes(const m3::State& state, const m3::Edge& edge)
{
    auto board = state.board;
    std::swap(board.cells.at(edge.swap.a), board.cells.at(edge.swap.b));
    const auto matches = m3::verification::matches(board);
    require(matches.size() == edge.refills.front().size(), "first wave differs from the certified witness");
    std::vector<int> counts(static_cast<std::size_t>(board.cols));
    for (const auto cell : matches) {
        ++counts.at(cell % board.cols);
    }
    std::string result;
    for (const auto size : counts) {
        if (!result.empty()) {
            result += ':';
        }
        result += std::to_string(size);
    }
    return result;
}

inline void profile(PreparedExpansion& data, std::uint64_t profile_seed)
{
    const auto& fixed = *data.board;
    const auto& oracle = *fixed.oracle;
    AgentChooser chooser(fixed.certificate, fixed.rules, fixed.board.seed);
    std::unordered_map<std::string, std::size_t> action_index;
    std::unordered_map<std::string, std::size_t> query_index;
    auto& stats = data.stats;

    for (int episode = 0; episode < profile_episodes_per_agent; ++episode) {
        for (int agent = 0; agent < static_cast<int>(agents.size()); ++agent) {
            auto current = oracle.root();
            bool goal_complete = false;
            for (int turn = 0; turn < fixed.board.moves; ++turn) {
                const auto base = mix(profile_seed ^ fixed.board.seed ^ mix(episode + 0x252a8174ULL) ^
                                      mix(turn + 0xec8bb24bULL));
                const auto& witness = chooser.choose(current.node(), agent, base);
                const auto& state = current.node().state;
                auto state_key = Bank::key(state);
                auto action_key = state_key;
                action_key += static_cast<char>(witness.swap.a);
                action_key += static_cast<char>(witness.swap.b);
                auto [action_it, added] = action_index.try_emplace(std::move(action_key), data.actions.size());
                const auto index = action_it->second;
                if (added) {
                    data.actions.push_back({current, &witness, fingerprint(state_key), column_holes(state, witness),
                                            bucket_index(current, witness)});
                }
                auto& action = data.actions.at(index);
                ++action.visits;
                ++stats.profile_turns;
                Step chosen = oracle.witness(current, witness);
                const auto attempts = adaptive_k(fixed, current, witness);
                for (int i = 0; i < attempts; ++i) {
                    const auto seed = mix(base ^ mix(i + 0x117677b8ULL));
                    auto observed = observe(oracle, current, witness, seed, nullptr);
                    ++action.proposals;
                    ++stats.profile_proposals;
                    const auto classified = category(observed.verdict);
                    record(action.profile, classified);
                    stats.profile_safe += classified == Category::Safe;
                    stats.profile_unsafe += classified == Category::Unsafe;
                    stats.profile_unresolved += classified == Category::Unresolved;
                    if (observed.draw.stop != Bank::Stop::stable) {
                        ++action.capped_or_depth;
                        continue;
                    }
                    const auto& next = observed.draw.state;
                    if (next.goal > 0 && next.moves > 0) {
                        auto next_key = Bank::key(next);
                        const auto occupied = refill_study::ProofBankExpansionAccess::lookup_indexed(oracle, next_key);
                        ++action.indexed_checks;
                        action.indexed_hits += occupied.has_value();
                        if (classified == Category::Unresolved) {
                            ++action.stable_unresolved;
                            ++stats.stable_unresolved;
                            auto [query_it, first] = query_index.try_emplace(next_key, data.queries.size());
                            if (first) {
                                auto id = fingerprint(next_key);
                                data.queries.push_back({index, std::move(next_key), std::move(id), std::move(observed.draw),
                                                        1, occupied.has_value()});
                            } else {
                                ++data.queries.at(query_it->second).frequency;
                                ++stats.repeated_hits;
                            }
                        }
                    }
                    if (classified == Category::Safe) {
                        chosen = Bank::replay(oracle, state, observed.draw, observed.proof);
                        break;
                    }
                }
                if (chosen.goal == 0) {
                    goal_complete = true;
                    break;
                }
                require(chosen.continuation.certificate, "profile continued without a proof");
                current = chosen.continuation;
            }
            require(goal_complete, "fixed bank profiling did not meet the Strict goal");
        }
    }
}

inline void audit_before(PreparedExpansion& data, std::uint64_t audit_seed)
{
    const auto domain = mix(audit_seed ^ data.board->board.seed ^ 0xf6211ee54b2c53efULL);
    for (std::size_t i = 0; i < data.actions.size(); ++i) {
        auto& action = data.actions.at(i);
        for (std::size_t draw = 0; draw < matched_draws_per_action; ++draw) {
            const auto seed = mix(domain ^ mix(i + 0x9212ULL) ^ mix(draw + 0x20f7ULL));
            const auto verdict = observe(*data.board->oracle, action.source, *action.witness, seed, nullptr).verdict;
            action.before_draws.at(draw) = category(verdict);
            record(action.before, category(verdict));
        }
    }
}

inline void expand(PreparedExpansion& data, std::ostream* evidence)
{
    auto& stats = data.stats;
    const auto& fixed = *data.board;
    const auto before = fixed.oracle->stats();
    std::vector<std::size_t> eligible;
    for (std::size_t i = 0; i < data.queries.size(); ++i) {
        auto& query = data.queries.at(i);
        if (query.base_indexed_unknown) {
            query.disposition = "same_budget_unknown_cached";
        } else {
            eligible.push_back(i);
        }
    }
    std::ranges::sort(eligible, [&](std::size_t left, std::size_t right) {
        const auto& a = data.queries.at(left);
        const auto& b = data.queries.at(right);
        return a.frequency != b.frequency ? a.frequency > b.frequency : a.successor_key < b.successor_key;
    });
    stats.prioritized_missing = static_cast<int>(eligible.size());
    for (std::size_t i = 0; i < eligible.size(); ++i) {
        auto& query = data.queries.at(eligible.at(i));
        query.rank = static_cast<int>(i + 1);
        if (data.variant == Variant::Base) {
            query.disposition = "base_control";
            continue;
        }
        if (i >= data.extra_limit) {
            query.disposition = "extra_cap";
            continue;
        }
        const auto& action = data.actions.at(query.action);
        const auto proof_number = static_cast<std::size_t>(stats.newly_verified + 1);
        const auto verdict = refill_study::ProofBankExpansionAccess::prove_missing(
            *fixed.oracle, action.source.node().state, query.natural, fixed.board.id, proof_number, evidence);
        ++stats.solver_calls;
        if (verdict == Outcome::safe_solved) {
            query.disposition = "verified_safe";
            query.proof_number = proof_number;
            ++stats.newly_verified;
        } else if (verdict == Outcome::unsafe_solved) {
            query.disposition = "fail_certified_not_indexed";
            ++stats.certified_fail;
        } else {
            require(verdict == Outcome::unknown_search, "unexpected continuation solver classification");
            query.disposition = "solver_unresolved";
            ++stats.solver_unresolved;
        }
    }
    const auto& after = fixed.oracle->stats();
    stats.proof_added_nodes = after.retained_certificate_nodes - before.retained_certificate_nodes;
    stats.extra_candidate_checks = after.candidate_checks - before.candidate_checks;
    stats.extra_refill_nodes = after.refill_nodes - before.refill_nodes;
    stats.extra_states = after.states - before.states;
    require(stats.solver_calls == stats.newly_verified + stats.certified_fail + stats.solver_unresolved &&
                stats.solver_calls <= static_cast<int>(data.extra_limit) &&
                after.solver_calls - before.solver_calls == static_cast<std::uint64_t>(stats.solver_calls) &&
                after.certificates_verified - before.certificates_verified == static_cast<std::uint64_t>(stats.newly_verified) &&
                after.natural_edges_replayed - before.natural_edges_replayed == static_cast<std::uint64_t>(stats.newly_verified),
            "offline expansion proof, solver, or replay counts disagree");
}

inline void audit_after(PreparedExpansion& data, std::uint64_t audit_seed)
{
    const auto domain = mix(audit_seed ^ data.board->board.seed ^ 0xf6211ee54b2c53efULL);
    std::array<Bucket, bucket_names.size()> audited{};
    for (std::size_t i = 0; i < data.actions.size(); ++i) {
        auto& action = data.actions.at(i);
        auto& bucket = audited.at(action.bucket);
        ++bucket.slots;
        ++audited.back().slots;
        for (std::size_t draw = 0; draw < matched_draws_per_action; ++draw) {
            const auto before = action.before_draws.at(draw);
            auto after = before;
            if (data.variant == Variant::Targeted) {
                const auto seed = mix(domain ^ mix(i + 0x9212ULL) ^ mix(draw + 0x20f7ULL));
                const auto verdict = observe(*data.board->oracle, action.source, *action.witness, seed, nullptr).verdict;
                after = category(verdict);
            }
            require(before == after || (before == Category::Unresolved && after == Category::Safe),
                    "adding safe proofs changed an existing admission or unsupported classification");
            ++action.transitions.at(static_cast<std::size_t>(before) * 3 + static_cast<std::size_t>(after));
            record(action.after, after);
            record(bucket.counts, after);
            record(audited.back().counts, after);
        }
    }
    if (data.variant == Variant::Base) {
        return;
    }
    for (auto& bucket : audited) {
        const auto samples = bucket.counts.safe + bucket.counts.unsafe + bucket.counts.unresolved;
        bucket.borrowed = samples < minimum_bucket_samples;
        bucket.estimate = acr::estimate_mass(bucket.borrowed ? audited.back().counts : bucket.counts,
                                             acr::MassSource::group_forecast);
        const auto plan = acr::choose_plan(bucket.estimate);
        bucket.k = static_cast<int>(plan.k);
        bucket.forecast_fallback = plan.fallback_probability;
        bucket.forecast_objective = plan.objective;
    }
    data.board->buckets = audited;
}

inline PreparedExpansion prepare_expanded_board(const fs::path& dataset, const BoardSpec& board,
                                                 std::uint64_t base_seed, std::uint64_t profile_seed,
                                                 std::uint64_t audit_seed, Variant variant,
                                                 std::uint32_t extra_limit = max_extra_solver_calls,
                                                 std::ostream* proof_sink = nullptr)
{
    require(extra_limit <= max_extra_solver_calls && base_seed != profile_seed &&
                base_seed != audit_seed && profile_seed != audit_seed,
            "expansion requires separate base/profile/audit seeds and an offline solve cap at most 240");
    PreparedExpansion result{};
    result.board = prepare_board(dataset, board, base_seed);
    result.variant = variant;
    result.extra_limit = extra_limit;
    const auto& base = result.board->oracle->stats();
    auto& stats = result.stats;
    stats.base_solver_calls = base.solver_calls;
    stats.base_verified = base.certificates_verified;
    stats.base_proof_nodes = base.retained_certificate_nodes;
    stats.base_cpu_ns = result.board->offline_cpu_ns;
    auto start = thread_cpu_ns();
    profile(result, profile_seed);
    stats.profile_cpu_ns = thread_cpu_ns() - start;
    start = thread_cpu_ns();
    audit_before(result, audit_seed);
    stats.audit_cpu_ns = thread_cpu_ns() - start;
    start = thread_cpu_ns();
    expand(result, proof_sink);
    stats.expansion_cpu_ns = thread_cpu_ns() - start;
    start = thread_cpu_ns();
    audit_after(result, audit_seed);
    stats.audit_cpu_ns += thread_cpu_ns() - start;
    return result;
}

inline void write_profile_csv_header(std::ostream& out)
{
    out << "bank,id,recipe,state_fp,swap_a,swap_b,bucket,remaining_goal,remaining_moves,first_wave_holes,column_holes,"
           "proof_origin,visits,proposals,prior_lookup_checks,prior_lookup_hits,prior_lookup_rate,"
           "safe,unsafe,unresolved,stable_unresolved,capped_or_depth\n";
}

inline void write_profile_csv_rows(std::ostream& out, const PreparedExpansion& data)
{
    for (const auto& action : data.actions) {
        const auto& state = action.source.node().state;
        out << variant_name(data.variant) << ',' << data.board->board.id << ',' << data.board->board.recipe << ','
            << action.fingerprint << ',' << action.witness->swap.a << ',' << action.witness->swap.b << ','
            << bucket_names.at(action.bucket) << ',' << state.goal << ',' << state.moves << ','
            << action.witness->refills.front().size() << ',' << action.column_holes << ','
            << (action.source.certificate == &data.board->certificate ? "original_witness" : "offline_continuation") << ','
            << action.visits << ',' << action.proposals << ',' << action.indexed_checks << ',' << action.indexed_hits << ',';
        if (action.indexed_checks) {
            out << static_cast<double>(action.indexed_hits) / action.indexed_checks;
        }
        out << ',' << action.profile.safe << ',' << action.profile.unsafe << ',' << action.profile.unresolved << ','
            << action.stable_unresolved << ',' << action.capped_or_depth << '\n';
    }
}

inline void write_queries_csv_header(std::ostream& out)
{
    out << "bank,id,recipe,state_fp,swap_a,swap_b,bucket,successor_fp,successor_goal,successor_moves,"
           "frequency,base_indexed_unknown,rank,disposition,proof_number\n";
}

inline void write_queries_csv_rows(std::ostream& out, const PreparedExpansion& data)
{
    for (const auto& query : data.queries) {
        const auto& action = data.actions.at(query.action);
        out << variant_name(data.variant) << ',' << data.board->board.id << ',' << data.board->board.recipe << ','
            << action.fingerprint << ',' << action.witness->swap.a << ',' << action.witness->swap.b << ','
            << bucket_names.at(action.bucket) << ',' << query.successor_fingerprint << ',' << query.natural.state.goal << ','
            << query.natural.state.moves << ',' << query.frequency << ',' << query.base_indexed_unknown << ',' << query.rank
            << ',' << query.disposition << ',' << query.proof_number << '\n';
    }
}

inline void write_expansion_csv_header(std::ostream& out)
{
    out << "bank,id,recipe,extra_limit,profile_episodes_per_agent,audit_draws_per_action,state_actions,profile_turns,"
           "profile_proposals,profile_safe,profile_unsafe,profile_unresolved,stable_unresolved,dedup_successors,"
           "repeated_hits,prioritized_missing,base_solver_calls,base_verified,base_proof_nodes,extra_solver_calls,"
           "newly_verified,certified_fail,solver_unresolved,proof_added_nodes,extra_candidate_checks,extra_refill_nodes,"
           "extra_states,base_cpu_ns,profile_cpu_ns,audit_cpu_ns,expansion_cpu_ns\n";
}

inline void write_expansion_csv_row(std::ostream& out, const PreparedExpansion& data)
{
    const auto& s = data.stats;
    out << variant_name(data.variant) << ',' << data.board->board.id << ',' << data.board->board.recipe << ','
        << data.extra_limit << ',' << profile_episodes_per_agent << ',' << matched_draws_per_action << ','
        << data.actions.size() << ',' << s.profile_turns << ',' << s.profile_proposals << ',' << s.profile_safe << ','
        << s.profile_unsafe << ',' << s.profile_unresolved << ',' << s.stable_unresolved << ',' << data.queries.size() << ','
        << s.repeated_hits << ',' << s.prioritized_missing << ',' << s.base_solver_calls << ',' << s.base_verified << ','
        << s.base_proof_nodes << ',' << s.solver_calls << ',' << s.newly_verified << ',' << s.certified_fail << ','
        << s.solver_unresolved << ',' << s.proof_added_nodes << ',' << s.extra_candidate_checks << ',' << s.extra_refill_nodes
        << ',' << s.extra_states << ',' << s.base_cpu_ns << ',' << s.profile_cpu_ns << ',' << s.audit_cpu_ns << ','
        << s.expansion_cpu_ns << '\n';
}

inline void write_audit_csv_header(std::ostream& out)
{
    out << "bank,id,recipe,state_fp,swap_a,swap_b,bucket,draws,base_safe,base_unsafe,base_unresolved,"
           "bank_safe,bank_unsafe,bank_unresolved,unknown_to_safe,unknown_to_unsafe,unchanged,"
           "safe_to_safe,safe_to_unsafe,safe_to_unknown,unsafe_to_safe,unsafe_to_unsafe,unsafe_to_unknown,unknown_to_unknown\n";
}

inline void write_audit_csv_rows(std::ostream& out, const PreparedExpansion& data)
{
    for (const auto& action : data.actions) {
        const auto& t = action.transitions;
        out << variant_name(data.variant) << ',' << data.board->board.id << ',' << data.board->board.recipe << ','
            << action.fingerprint << ',' << action.witness->swap.a << ',' << action.witness->swap.b << ','
            << bucket_names.at(action.bucket) << ',' << matched_draws_per_action << ',' << action.before.safe << ','
            << action.before.unsafe << ',' << action.before.unresolved << ',' << action.after.safe << ','
            << action.after.unsafe << ',' << action.after.unresolved << ',' << t.at(6) << ',' << t.at(7) << ','
            << t.at(0) + t.at(4) + t.at(8) << ',' << t.at(0) << ',' << t.at(1) << ',' << t.at(2) << ',' << t.at(3)
            << ',' << t.at(4) << ',' << t.at(5) << ',' << t.at(8) << '\n';
    }
}

} // namespace proofbank
} // namespace
