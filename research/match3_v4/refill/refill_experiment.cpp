#define main old_simulator_main
#include "simulate_for_refill.inc"
#undef main

#include "refill_oracle.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <tuple>

namespace {

using refill_study::Oracle;
using refill_study::Outcome;
using refill_study::Probe;
using refill_study::ProofRef;
using refill_study::Step;

constexpr std::array controller_names{"Witness", "ConditionalRandom", "MinIntervention"};
constexpr int wave_count_cap = 9;
constexpr int wave_size_cap = 12;

struct BoardSpec {
    std::string id;
    std::uint64_t seed;
    std::string recipe;
    int goal;
    int moves;
};

std::vector<BoardSpec> read_boards(const fs::path& dataset, int per_recipe)
{
    std::ifstream source(dataset / "candidates.csv");
    require(static_cast<bool>(source), "cannot open candidate list");
    std::unordered_map<std::string, std::size_t> columns;
    std::string line;
    require(static_cast<bool>(std::getline(source, line)), "empty candidate list");
    for (const auto& field : csv_fields(line)) {
        columns.emplace(field, columns.size());
    }
    std::vector<BoardSpec> boards;
    std::unordered_set<std::string> ids;
    std::map<std::string, int> counts;
    while (std::getline(source, line)) {
        const auto fields = csv_fields(line);
        const auto field = [&](const char* key) -> const std::string& { return fields.at(columns.at(key)); };
        require(ids.insert(field("id")).second, "duplicate candidate ID");
        const bool win = field("verdict") == "WIN_CERTIFIED";
        require(win == (field("cert_verified") == "1"), "candidate verdict disagrees with verified flag");
        if (!win) {
            continue;
        }
        auto& count = counts[field("stratum")];
        if (count >= per_recipe) {
            continue;
        }
        ++count;
        boards.push_back({field("id"), std::stoull(field("seed")), field("stratum"),
                          std::stoi(field("goal")), std::stoi(field("moves"))});
    }
    require(counts.size() == 4, "expected four tested recipes");
    for (const auto& [recipe, count] : counts) {
        require(count == per_recipe, "too few certified boards in " + recipe);
    }
    return boards;
}

std::uint64_t hash_state(const std::string& bytes)
{
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto byte : bytes) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 1099511628211ULL;
    }
    return mix(hash);
}

class AgentChooser {
public:
    AgentChooser(const m3::Certificate& source, const m3::Rules& rules, std::uint64_t seed)
        : source_(source), rules_(rules), seed_(seed)
    {
        for (std::size_t index = 0; index < source.nodes.size(); ++index) {
            const auto& state = source.nodes[index].state;
            if (state.goal > 0) {
                // Distinct proof nodes can describe the same playable state.
                // A controller must not select different heuristic probe RNGs
                // solely because that state came from a different proof path.
                original_.try_emplace(state_key(state.board, state.goal, state.moves), index);
            }
        }
        for (std::size_t agent = 0; agent < agents.size(); ++agent) {
            policies_[agent] = std::make_unique<Policy>(source, rules, seed, agent);
        }
    }

    Policy& original_policy(int agent) { return *policies_.at(agent); }

    const std::vector<m3::Swap>& best_swaps(const m3::CertificateNode& node, int agent)
    {
        const auto& state = node.state;
        const auto key = state_key(state.board, state.goal, state.moves);
        auto& cache = choices_.at(agent);
        const auto [found, inserted] = cache.try_emplace(key);
        if (inserted) {
            auto& choices = found->second;
            if (const auto prior = original_.find(key); prior != original_.end()) {
                const auto original_index = prior->second;
                for (const auto index : original_policy(agent).refill_choices(original_index).edges) {
                    choices.push_back(source_.nodes.at(original_index).edges.at(index).swap);
                }
            } else {
                choices = evaluate(node, agent, hash_state(key));
            }
            std::ranges::sort(choices, {}, [](const m3::Swap& swap) { return std::pair{swap.a, swap.b}; });
            require(!choices.empty(), "agent found no playable swap");
        }
        return found->second;
    }

    const m3::Edge& choose(const m3::CertificateNode& node, int agent, std::uint64_t seed)
    {
        const auto& choices = best_swaps(node, agent);
        std::mt19937_64 random(seed);
        const auto selected = choices[m3::board_detail::uniform_below(random, choices.size())];
        const auto edge = std::ranges::find(node.edges, selected, &m3::Edge::swap);
        require(edge != node.edges.end(), "chosen swap is missing from the current certificate");
        return *edge;
    }

private:
    std::vector<m3::Swap> evaluate(const m3::CertificateNode& node, int agent, std::uint64_t salt) const
    {
        std::vector<m3::Swap> best;
        std::optional<std::array<int, 3>> best_value;
        for (const auto& edge : node.edges) {
            const auto swap = edge.swap;
            std::array<int, 3> value{};
            if (agent == 1) {
                value[0] = immediate_clears(node.state.board, swap);
            } else if (agent > 1) {
                const auto probe_seed = mix(seed_ ^ 0xdd54aa90ULL ^ salt ^ (swap.a * 257 + swap.b));
                const auto score = evaluate_choice(node.state, swap, rules_, probe_seed, agent == 3);
                value = agent == 2 ? std::array{score.progress, score.finish, -score.overshoot}
                                   : std::array{score.lookahead, 0, 0};
            }
            if (!best_value || value > *best_value) {
                best_value = value;
                best.clear();
            }
            if (value == *best_value) {
                best.push_back(swap);
            }
        }
        return best;
    }

    const m3::Certificate& source_;
    const m3::Rules& rules_;
    std::uint64_t seed_;
    std::unordered_map<std::string, std::size_t> original_;
    std::array<std::unique_ptr<Policy>, agents.size()> policies_;
    std::array<std::unordered_map<std::string, std::vector<m3::Swap>>, agents.size()> choices_;
};

struct Counts {
    int episodes{}, wins{}, early{}, challenge{}, chain{}, productive_extra{}, clears{};
    int turns{}, witness{}, natural{}, first_accepted{}, fallback{};
    int proposals{}, safe{}, unsafe{}, unknown{}, cap{}, depth{}, same_witness{};
    int solver_calls{}, cache_hits{}, extra_waves{}, max_waves{};
    std::array<std::array<int, wave_size_cap + 1>, wave_count_cap> wave_sizes{};
};

struct Trial {
    int turns{}, clears{}, left{}, productive{}, extra_waves{}, max_waves{}, proposals{}, fallbacks{};
};

void record_proposal(Counts& counts, const Probe& probe, bool first)
{
    ++counts.proposals;
    counts.cache_hits += probe.cache_hit;
    counts.solver_calls += probe.solver_called;
    counts.safe += probe.accepted();
    counts.first_accepted += first && probe.accepted();
    counts.same_witness += probe.accepted() && probe.same_witness;
    counts.cap += probe.outcome == Outcome::unsafe_refill_cap;
    counts.depth += probe.outcome == Outcome::unknown_depth;
    const bool unknown = probe.outcome == Outcome::unknown_depth ||
                         probe.outcome == Outcome::unknown_search ||
                         probe.outcome == Outcome::unknown_work_cap;
    counts.unknown += unknown;
    counts.unsafe += !probe.accepted() && !unknown;
}

Trial play(Oracle& oracle, AgentChooser& chooser, const BoardSpec& board, int agent, int attempts,
           int run, std::uint64_t sample_seed, Counts& counts)
{
    auto current = oracle.root();
    require(current.node().state.goal == board.goal, "invalid root goal");
    Trial trial;
    while (trial.turns < board.moves) {
        const auto& node = current.node();
        const auto& state = node.state;
        const auto usual = mix(sample_seed ^ board.seed ^ mix(static_cast<std::uint64_t>(run) + 0x252a8174ULL)
                               ^ mix(static_cast<std::uint64_t>(trial.turns) + 0xec8bb24bULL));
        const auto& selected = chooser.choose(node, agent, usual);
        std::optional<Step> step;
        for (int attempt = 0; attempt < attempts; ++attempt) {
            const auto proposal_seed = mix(usual ^ mix(static_cast<std::uint64_t>(attempt) + 0x117677b8ULL));
            auto probe = oracle.probe(current, selected, proposal_seed);
            record_proposal(counts, probe, attempt == 0);
            ++trial.proposals;
            if (probe.accepted()) {
                ++counts.natural;
                step = std::move(probe.step);
                break;
            }
        }
        if (!step) {
            counts.fallback += attempts > 0;
            trial.fallbacks += attempts > 0;
            ++counts.witness;
            step = oracle.witness(current, selected);
        }

        require(step->waves > 0 && step->waves <= wave_count_cap &&
                    static_cast<int>(step->wave_sizes.size()) == step->waves &&
                    std::accumulate(step->wave_sizes.begin(), step->wave_sizes.end(), 0) == step->clear,
                "replayed wave sizes inconsistent with executed turn");
        require(step->goal == std::max(0, state.goal - step->clear) && step->moves == state.moves - 1,
                "replayed objective or turn limit inconsistent with executed turn");
        for (int wave = 0; wave < step->waves; ++wave) {
            ++counts.wave_sizes.at(wave).at(step->wave_sizes.at(wave));
        }
        ++trial.turns;
        trial.clears += step->clear;
        trial.productive += std::clamp(state.goal - step->wave_sizes.front(), 0, step->extra);
        trial.extra_waves += step->waves - 1;
        trial.max_waves = std::max(trial.max_waves, step->waves);
        if (step->goal == 0) {
            trial.left = step->moves;
            ++counts.wins;
            break;
        }
        require(step->continuation.certificate != nullptr && step->moves > 0,
                "a played nonterminal successor has no verified continuation");
        current = step->continuation;
        require(current.node().state.goal == step->goal && current.node().state.moves == step->moves,
                "continuation does not match played successor");
    }
    require(trial.clears >= board.goal && trial.productive <= board.goal, "episode failed Strict goal or exceeded productive clears");
    ++counts.episodes;
    counts.early += trial.left > 0;
    counts.challenge += trial.clears >= board.goal + challenge_extra;
    counts.chain += trial.extra_waves > 0;
    counts.productive_extra += trial.productive;
    counts.clears += trial.clears;
    counts.turns += trial.turns;
    counts.extra_waves += trial.extra_waves;
    counts.max_waves = std::max(counts.max_waves, trial.max_waves);
    return trial;
}

double exact_productive(const m3::Certificate& cert, Policy& policy)
{
    std::vector<double> memo(cert.nodes.size(), -1);
    const auto run = [&](auto&& self, std::size_t index) -> double {
        auto& cache = memo.at(index);
        if (cache >= 0) {
            return cache;
        }
        const auto& node = cert.nodes.at(index);
        cache = 0;
        if (node.state.goal > 0) {
            const auto& choices = policy.refill_choices(index).edges;
            for (const auto chosen : choices) {
                const auto& edge = node.edges.at(chosen);
                const auto first = static_cast<int>(edge.refills.front().size());
                const auto extra = edge_clears(edge) - first;
                cache += (std::clamp(node.state.goal - first, 0, extra) + self(self, edge.child)) / choices.size();
            }
        }
        return cache;
    };
    return run(run, cert.root);
}

struct ExactWitness {
    double early{}, challenge{}, chain{}, productive{};
};

ExactWitness exact_comparator_witness(const m3::Certificate& cert, AgentChooser& chooser, int agent)
{
    std::vector<std::optional<ExactWitness>> memo(cert.nodes.size());
    const auto run = [&](auto&& self, std::size_t index) -> ExactWitness {
        auto& cache = memo.at(index);
        if (cache) {
            return *cache;
        }
        const auto& node = cert.nodes.at(index);
        auto& total = cache.emplace();
        if (node.state.goal == 0) {
            total.early = node.state.moves > 0;
        } else {
            const auto& choices = chooser.best_swaps(node, agent);
            const double probability = 1.0 / choices.size();
            for (const auto swap : choices) {
                const auto edge = std::ranges::find(node.edges, swap, &m3::Edge::swap);
                require(edge != node.edges.end(), "policy swap is missing from original certificate");
                const auto first = static_cast<int>(edge->refills.front().size());
                const auto all = edge_clears(*edge);
                const auto extra = all - first;
                const auto child = self(self, edge->child);
                total.early += probability * child.early;
                total.challenge += probability * (cert.nodes.at(edge->child).state.goal == 0
                    ? all >= node.state.goal + challenge_extra : child.challenge);
                total.chain += probability * (edge->refills.size() > 1 ? 1 : child.chain);
                total.productive += probability * (std::clamp(node.state.goal - first, 0, extra) + child.productive);
            }
        }
        return total;
    };
    return run(run, cert.root);
}

struct BoardOutput {
    std::string summary, episodes, exact, waves, oracle;
};

BoardOutput evaluate_board(const fs::path& dataset, const BoardSpec& board, int runs, int conditional_attempts,
                           std::uint64_t sample_seed, std::size_t max_solver_calls)
{
    const auto started = std::chrono::steady_clock::now();
    std::ifstream file(dataset / "certificates" / (board.id + ".m3cert"), std::ios::binary);
    require(static_cast<bool>(file), "can't read certificate " + board.id);
    const std::string certificate{std::istreambuf_iterator<char>(file), {}};
    const auto problem = read_problem(dataset / "boards" / (board.id + ".txt"), board.goal, board.moves, wave_size_cap);
    require(strict::verify_certificate(problem, certificate, wave_count_cap), "source certificate verification failed: " + board.id);
    std::istringstream encoded(certificate);
    const auto [rules, cert] = m3::read_certificate(encoded);
    require(rules.refill_cap == wave_size_cap && rules.cascade_budget == wave_count_cap, "unexpected replay rules");
    const strict::SearchBudget budget{16'000, 45'000, 600, wave_count_cap};
    Oracle oracle(cert, rules, budget, max_solver_calls);
    AgentChooser chooser(cert, rules, board.seed);

    std::ostringstream summary, episodes, exact, waves, oracle_row;
    exact << std::setprecision(15);
    const std::array attempts{0, conditional_attempts, 1};
    for (int agent = 0; agent < static_cast<int>(agents.size()); ++agent) {
        auto& policy = chooser.original_policy(agent);
        const auto [challenge, early, chain] = policy.exact(challenge_extra);
        const auto paired_exact = exact_comparator_witness(cert, chooser, agent);
        exact << board.id << ',' << agents[agent] << ',' << early << ',' << challenge << ',' << chain
              << ',' << exact_productive(cert, policy) << ',' << paired_exact.early << ',' << paired_exact.challenge
              << ',' << paired_exact.chain << ',' << paired_exact.productive << '\n';
        for (std::size_t ctl = 0; ctl < controller_names.size(); ++ctl) {
            const auto controller = controller_names[ctl];
            Counts counts;
            for (int run = 0; run < runs; ++run) {
                const auto trial = play(oracle, chooser, board, agent, attempts[ctl], run, sample_seed, counts);
                episodes << board.id << ',' << agents[agent] << ',' << controller << ',' << run << ',' << trial.turns
                         << ',' << trial.clears << ',' << trial.left << ',' << (trial.extra_waves > 0) << ','
                         << trial.productive << ',' << trial.extra_waves << ',' << trial.max_waves << ','
                         << trial.proposals << ',' << trial.fallbacks << '\n';
            }
            require(counts.episodes == runs && counts.wins == runs && counts.turns == counts.witness + counts.natural &&
                        counts.proposals == counts.safe + counts.unsafe + counts.unknown && counts.safe == counts.natural,
                    "controller counters do not reconcile");
            summary << board.id << ',' << board.seed << ',' << board.recipe << ',' << board.goal << ',' << board.moves << ','
                    << agents[agent] << ',' << controller << ',' << counts.episodes << ',' << counts.wins << ',' << counts.early
                    << ',' << counts.challenge << ',' << counts.chain << ',' << counts.productive_extra << ',' << counts.clears
                    << ',' << counts.turns << ',' << counts.witness << ',' << counts.natural << ',' << counts.first_accepted
                    << ',' << counts.fallback << ',' << counts.proposals << ',' << counts.safe << ',' << counts.unsafe << ','
                    << counts.unknown << ',' << counts.cap << ',' << counts.depth << ',' << counts.same_witness << ','
                    << counts.solver_calls << ',' << counts.cache_hits << ',' << counts.extra_waves << ',' << counts.max_waves << '\n';
            for (int wave = 0; wave < wave_count_cap; ++wave) {
                for (int size = 0; size <= wave_size_cap; ++size) {
                    const auto count = counts.wave_sizes.at(wave).at(size);
                    if (count != 0) {
                        waves << board.id << ',' << agents[agent] << ',' << controller << ',' << wave + 1 << ',' << size << ',' << count << '\n';
                    }
                }
            }
        }
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    const auto& stats = oracle.stats();
    require(stats.proposals == std::accumulate(stats.outcomes.begin(), stats.outcomes.end(), std::uint64_t{}) &&
                stats.natural_edges_replayed == stats.outcomes[static_cast<std::size_t>(Outcome::safe_terminal)] +
                    stats.outcomes[static_cast<std::size_t>(Outcome::safe_known)] +
                    stats.outcomes[static_cast<std::size_t>(Outcome::safe_solved)],
            "oracle replay counters do not reconcile");
    oracle_row << board.id << ',' << elapsed << ',' << stats.proposals << ',' << stats.cache_hits << ','
               << stats.solver_calls << ',' << stats.certificates_verified << ',' << stats.natural_edges_replayed
               << ',' << stats.same_witness << ',' << stats.candidate_checks << ',' << stats.refill_nodes
               << ',' << stats.states << ',' << stats.candidate_cutoffs << ',' << stats.refill_cutoffs
               << ',' << stats.state_cutoffs << ',' << stats.cascade_cutoffs << ',' << stats.certificate_limits
               << ',' << stats.retained_certificate_nodes;
    for (const auto count : stats.outcomes) {
        oracle_row << ',' << count;
    }
    oracle_row << '\n';
    return {summary.str(), episodes.str(), exact.str(), waves.str(), oracle_row.str()};
}

} // namespace

int main(int argc, char** argv) try {
    require(argc == 8 || argc == 9,
            "usage: refill-experiment DATASET OUTPUT EPISODES PER_RECIPE WORKERS CONDITIONAL_ATTEMPTS SAMPLE_SEED [BOARD_SOLVE_CAP]");
    const fs::path dataset = argv[1];
    const fs::path output = argv[2];
    const int runs = std::stoi(argv[3]);
    const int per_recipe = std::stoi(argv[4]);
    const int workers = std::stoi(argv[5]);
    const int attempts = std::stoi(argv[6]);
    const auto sample_seed = std::stoull(argv[7], nullptr, 0);
    const auto max_solver_calls = argc == 9 ? std::stoull(argv[8]) : 0;
    require(runs > 0 && per_recipe > 0 && workers > 0 && attempts > 1,
            "positive runs/boards/workers and at least two conditional draws required");
    require(!fs::exists(output), "output directory must not exist");
    const auto boards = read_boards(dataset, per_recipe);
    std::vector<BoardOutput> results(boards.size());
    std::atomic_size_t next{};
    std::atomic_size_t completed{};
    const auto started = std::chrono::steady_clock::now();
    std::vector<std::future<void>> threads;
    for (int worker = 0; worker < workers; ++worker) {
        threads.push_back(std::async(std::launch::async, [&] {
            for (auto index = next++; index < boards.size(); index = next++) {
                try {
                    results[index] = evaluate_board(dataset, boards[index], runs, attempts, sample_seed, max_solver_calls);
                } catch (const std::exception& error) {
                    throw std::runtime_error(boards[index].id + ": " + error.what());
                }
                auto done = ++completed;
                if (done % 10 == 0 || done == boards.size()) {
                    std::cerr << "simulated " << done << '/' << boards.size() << " boards\n";
                }
            }
        }));
    }
    for (auto& thread : threads) {
        thread.get();
    }

    fs::create_directories(output);
    std::ofstream summary(output / "summary.csv"), episodes(output / "episodes.csv"), exact(output / "witness-exact.csv"),
        waves(output / "wave-sizes.csv"), oracle(output / "oracle-board.csv");
    require(summary && episodes && exact && waves && oracle, "cannot create controller comparison tables");
    summary << "id,seed,recipe,goal,moves,agent,controller,episodes,wins,early,challenge,chain,productive_extra,clears,"
               "total_turns,executed_witness,executed_proposal,first_accepted,witness_fallbacks,proposal_attempts,proposal_safe,"
               "proposal_unsafe,proposal_unknown,proposal_cap,proposal_depth,proposal_same_witness,solver_calls,cache_hits,"
               "extra_waves,max_waves\n";
    episodes << "id,agent,controller,run,turns,clears,left,chain,productive_extra,extra_waves,max_waves,proposals,fallbacks\n";
    exact << "id,agent,exact_early,exact_challenge,exact_chain,exact_mean_productive_extra_tiles,"
             "paired_exact_early,paired_exact_challenge,paired_exact_chain,paired_exact_mean_productive_extra_tiles\n";
    waves << "id,agent,controller,wave,tiles,count\n";
    oracle << "id,elapsed_ms,proposals,cache_hits,solver_calls,certificates_verified,natural_edges_replayed,same_witness,"
              "candidate_checks,refill_nodes,states,candidate_cutoffs,refill_cutoffs,state_cutoffs,cascade_cutoffs,"
              "certificate_limits,retained_certificate_nodes";
    for (const auto name : refill_study::outcome_names) {
        oracle << ',' << name;
    }
    oracle << '\n';
    for (const auto& result : results) {
        summary << result.summary;
        episodes << result.episodes;
        exact << result.exact;
        waves << result.waves;
        oracle << result.oracle;
    }
    summary.close();
    episodes.close();
    exact.close();
    waves.close();
    oracle.close();
    require(summary && episodes && exact && waves && oracle, "incomplete controller comparison output");
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::cerr << "completed " << boards.size() << " boards in " << seconds << " s\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "Refill experiment failed: " << error.what() << '\n';
    return 1;
}
