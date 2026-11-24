#include "refill_for_acr.inc"
#include "acr_controller.hpp"

#include <ctime>
#include <limits>

namespace refill_study {

// This reader has no method that solves or mutates the offline bank. The build
// adapter grants access to the prior oracle's audited draw/replay implementation.
struct OfflineBankAccess {
    using Entry = Oracle::CacheEntry;
    using NaturalTurn = Oracle::NaturalTurn;
    using Stop = Oracle::Stop;

    static NaturalTurn draw(const Oracle& bank, const m3::State& state, m3::Swap swap, std::uint64_t seed)
    {
        return bank.draw_turn(state, swap, seed);
    }

    static std::string key(const m3::State& state) { return Oracle::key(state); }

    static Entry lookup(const Oracle& bank, const m3::State& state, const std::string& key)
    {
        if (const auto root = bank.known_root_.find(key); root != bank.known_root_.end()) {
            return {Outcome::safe_known, root->second};
        }
        if (const auto found = bank.cache_.find(key); found != bank.cache_.end()) {
            return found->second;
        }
        if (m3::legal_swaps(state.board).empty()) {
            return {Outcome::unsafe_dead, {}};
        }
        return {Outcome::unknown_search, {}};
    }

    static Step replay(const Oracle& bank, const m3::State& parent, const NaturalTurn& natural, ProofRef proof)
    {
        require(m3::verification::replay_edge(parent, bank.rules_, natural.edge, natural.state),
                "frozen-bank proposal failed independent replay");
        require(natural.state.goal == 0 || (proof.certificate && proof.node().state == natural.state),
                "frozen-bank proposal lacks its exact verified continuation");
        return Oracle::make_step(natural.state, natural.edge, proof);
    }
};

} // namespace refill_study

namespace {

using Bank = refill_study::OfflineBankAccess;

constexpr std::uint64_t cpu_limit_ns = 2'000'000;
constexpr int candidate_limit = 512;
constexpr int refill_node_limit = 8192;
constexpr int state_limit = 128;
constexpr int max_proposal_refill_nodes = wave_count_cap * wave_size_cap;
constexpr int root_offline_draws = 12;
constexpr int offline_episodes_per_agent = 16;
constexpr int offline_draws_per_turn = 3;
constexpr int audit_draws_per_action = 12;
constexpr int max_audit_actions = 160;
constexpr int minimum_bucket_samples = 24;
constexpr std::size_t offline_solve_limit = 400;
constexpr std::array<std::string_view, 6> bucket_names{
    "last_finish", "last_short", "multi_finish", "multi_tight", "multi_slack", "all",
};

std::uint64_t thread_cpu_ns()
{
    timespec stamp{};
    require(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &stamp) == 0, "thread CPU clock unavailable");
    return static_cast<std::uint64_t>(stamp.tv_sec) * 1'000'000'000ULL + static_cast<std::uint64_t>(stamp.tv_nsec);
}

bool is_safe(Outcome outcome)
{
    return outcome == Outcome::safe_terminal || outcome == Outcome::safe_known || outcome == Outcome::safe_solved;
}

bool is_unresolved(Outcome outcome)
{
    return outcome == Outcome::unknown_depth || outcome == Outcome::unknown_search || outcome == Outcome::unknown_work_cap;
}

struct LookupCache {
    std::unordered_map<std::string, Bank::Entry> entries;

    Bank::Entry find(const Oracle& bank, const m3::State& state, bool& hit)
    {
        auto key = Bank::key(state);
        const auto it = entries.find(key);
        if (it != entries.end()) {
            hit = true;
            return it->second;
        }
        const auto result = Bank::lookup(bank, state, key);
        entries.emplace(std::move(key), result);
        return result;
    }
};

struct FrozenObservation {
    Bank::NaturalTurn draw;
    Outcome verdict;
    ProofRef proof;
    bool cache_hit{};
};

FrozenObservation observe(const Oracle& bank, ProofRef current, const m3::Edge& witness,
                          std::uint64_t seed, LookupCache* cache)
{
    auto draw = Bank::draw(bank, current.node().state, witness.swap, seed);
    auto outcome = Outcome::unknown_search;
    ProofRef proof{};
    bool cache_hit = false;
    if (draw.stop == Bank::Stop::refill_cap) {
        outcome = Outcome::unsafe_refill_cap;
    } else if (draw.stop == Bank::Stop::depth) {
        outcome = Outcome::unknown_depth;
    } else if (draw.state.goal == 0) {
        outcome = Outcome::safe_terminal;
    } else if (draw.state.moves == 0) {
        outcome = Outcome::unsafe_no_moves;
    } else if (const ProofRef child{current.certificate, witness.child}; child.node().state == draw.state) {
        outcome = Outcome::safe_known;
        proof = child;
    } else {
        const auto result = cache ? cache->find(bank, draw.state, cache_hit)
                                  : Bank::lookup(bank, draw.state, Bank::key(draw.state));
        outcome = result.outcome;
        proof = result.proof;
    }
    return {std::move(draw), outcome, proof, cache_hit};
}

struct TurnChoice {
    Step step;
    std::vector<std::vector<std::uint8_t>> refills;
    int candidate_checks{}, refill_nodes{}, states{}, cache_hits{}, safe{}, unsafe{}, unresolved{};
    bool used_witness{}, fallback{}, timeout{}, capped{}, same_witness{};
    std::uint64_t cpu_ns{}, wall_ns{};
};

TurnChoice choose_refill(const Oracle& bank, ProofRef current, const m3::Edge& witness, int k,
                         std::uint64_t proposal_seed, LookupCache* cache)
{
    const auto wall_start = std::chrono::steady_clock::now();
    const auto cpu_start = thread_cpu_ns();
    TurnChoice result;
    for (int attempt = 0; attempt < k; ++attempt) {
        if (thread_cpu_ns() - cpu_start >= cpu_limit_ns) {
            result.timeout = true;
            break;
        }
        // Reserve the worst-case cost of one complete quota trajectory. No
        // solver expansion takes place online; a state is an endpoint check.
        if (result.candidate_checks == candidate_limit || result.states == state_limit ||
            result.refill_nodes + max_proposal_refill_nodes > refill_node_limit) {
            result.capped = true;
            break;
        }
        const auto seed = mix(proposal_seed ^ mix(static_cast<std::uint64_t>(attempt) + 0x117677b8ULL));
        auto trial = observe(bank, current, witness, seed, cache);
        ++result.candidate_checks;
        result.states += trial.draw.stop == Bank::Stop::stable;
        for (const auto& wave : trial.draw.edge.refills) {
            result.refill_nodes += static_cast<int>(wave.size());
        }
        result.cache_hits += trial.cache_hit;
        if (is_safe(trial.verdict)) {
            ++result.safe;
            auto accepted = Bank::replay(bank, current.node().state, trial.draw, trial.proof);
            if (thread_cpu_ns() - cpu_start >= cpu_limit_ns) {
                result.timeout = true;
                break;
            }
            result.step = std::move(accepted);
            result.same_witness = trial.draw.edge.refills == witness.refills;
            result.refills = std::move(trial.draw.edge.refills);
            break;
        }
        if (is_unresolved(trial.verdict)) {
            ++result.unresolved;
        } else {
            ++result.unsafe;
        }
        if (thread_cpu_ns() - cpu_start >= cpu_limit_ns) {
            result.timeout = true;
            break;
        }
    }
    if (result.refills.empty()) {
        result.used_witness = true;
        result.fallback = k > 0;
        result.step = bank.witness(current, witness);
        result.refills = witness.refills;
    }
    result.cpu_ns = thread_cpu_ns() - cpu_start;
    result.wall_ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - wall_start).count());
    require(result.safe + result.unsafe + result.unresolved == result.candidate_checks, "proposal counts differ");
    require(result.candidate_checks <= candidate_limit && result.refill_nodes <= refill_node_limit &&
                result.states <= state_limit, "online work cap exceeded");
    return result;
}

struct Bucket {
    acr::SafetyCounts counts{};
    int slots{};
    acr::MassEstimate estimate{};
    int k{};
    double forecast_fallback{}, forecast_objective{};
    bool borrowed{};
};

struct PreparedBoard {
    BoardSpec board;
    m3::Rules rules;
    m3::Certificate certificate;
    std::unique_ptr<Oracle> oracle;
    std::array<Bucket, bucket_names.size()> buckets{};
    std::uint64_t offline_wall_ns{}, offline_cpu_ns{};
};

int bucket_index(ProofRef current, const m3::Edge& witness)
{
    const auto& state = current.node().state;
    const auto first = static_cast<int>(witness.refills.front().size());
    if (state.moves == 1) {
        return first >= state.goal ? 0 : 1;
    }
    if (first >= state.goal) {
        return 2;
    }
    return state.goal - first > 3 * (state.moves - 1) ? 3 : 4;
}

int adaptive_k(const PreparedBoard& prepared, ProofRef current, const m3::Edge& witness)
{
    return prepared.buckets.at(bucket_index(current, witness)).k;
}

struct AuditAction {
    ProofRef source;
    const m3::Edge* witness;
};

void record_bucket(Bucket& bucket, Outcome outcome)
{
    if (is_safe(outcome)) {
        ++bucket.counts.safe;
    } else if (is_unresolved(outcome)) {
        ++bucket.counts.unresolved;
    } else {
        ++bucket.counts.unsafe;
    }
}

void build_bank(PreparedBoard& prepared, std::uint64_t study_seed)
{
    auto& oracle = *prepared.oracle;
    const auto root = oracle.root();
    AgentChooser chooser(prepared.certificate, prepared.rules, prepared.board.seed);
    std::vector<AuditAction> actions;
    std::unordered_set<std::string> registered;
    const auto register_action = [&](ProofRef current, const m3::Edge& witness) {
        if (actions.size() >= max_audit_actions) {
            return;
        }
        auto key = Bank::key(current.node().state);
        key += static_cast<char>(witness.swap.a);
        key += static_cast<char>(witness.swap.b);
        if (registered.insert(std::move(key)).second) {
            actions.push_back({current, &witness});
        }
    };
    const auto discover = mix(study_seed ^ prepared.board.seed ^ 0xd40dcd58d7a89ac1ULL);
    for (std::size_t edge = 0; edge < root.node().edges.size(); ++edge) {
        const auto& witness = root.node().edges.at(edge);
        register_action(root, witness);
        for (int i = 0; i < root_offline_draws; ++i) {
            (void) oracle.probe(root, witness, mix(discover ^ mix(edge + 0xaac34ULL) ^ mix(i + 0x10fbULL)));
        }
    }
    for (int episode = 0; episode < offline_episodes_per_agent; ++episode) {
        for (int agent = 0; agent < static_cast<int>(agents.size()); ++agent) {
            auto current = root;
            for (int turn = 0; turn < prepared.board.moves; ++turn) {
                const auto base = mix(discover ^ 0x3fe7c68f1b3da761ULL ^ mix(episode + 0x12f3ULL) ^
                                      mix(agent + 0xa340ULL) ^ mix(turn + 0x5c9ULL));
                const auto& witness = chooser.choose(current.node(), agent, base);
                register_action(current, witness);
                auto chosen = oracle.witness(current, witness);
                for (int attempt = 0; attempt < offline_draws_per_turn; ++attempt) {
                    auto probe = oracle.probe(current, witness, mix(base ^ mix(attempt + 0x1329ULL)));
                    if (probe.accepted()) {
                        chosen = std::move(probe.step);
                        break;
                    }
                }
                if (chosen.goal == 0) {
                    break;
                }
                current = chosen.continuation;
            }
        }
    }

    // Independent probes estimate the mass supported by the frozen bank. No
    // classify call below can discover, solve, or add a proof.
    const auto audit = mix(study_seed ^ prepared.board.seed ^ 0xe33d2abc90b5a77dULL);
    for (std::size_t i = 0; i < actions.size(); ++i) {
        const auto& entry = actions.at(i);
        auto& specific = prepared.buckets.at(bucket_index(entry.source, *entry.witness));
        auto& all = prepared.buckets.back();
        ++specific.slots;
        ++all.slots;
        for (int draw = 0; draw < audit_draws_per_action; ++draw) {
            const auto seed = mix(audit ^ mix(i + 0x7a21ULL) ^ mix(draw + 0x35cULL));
            const auto outcome = observe(oracle, entry.source, *entry.witness, seed, nullptr).verdict;
            record_bucket(specific, outcome);
            record_bucket(all, outcome);
        }
    }
    for (auto& bucket : prepared.buckets) {
        const auto samples = bucket.counts.safe + bucket.counts.unsafe + bucket.counts.unresolved;
        bucket.borrowed = samples < minimum_bucket_samples;
        const auto& effective = bucket.borrowed ? prepared.buckets.back().counts : bucket.counts;
        bucket.estimate = acr::estimate_mass(effective, acr::MassSource::group_forecast);
        const auto plan = acr::choose_plan(bucket.estimate);
        bucket.k = static_cast<int>(plan.k);
        bucket.forecast_fallback = plan.fallback_probability;
        bucket.forecast_objective = plan.objective;
    }
}

std::unique_ptr<PreparedBoard> prepare_board(const fs::path& dataset, const BoardSpec& board, std::uint64_t study_seed)
{
    const auto before = std::chrono::steady_clock::now();
    const auto cpu_before = thread_cpu_ns();
    auto prepared = std::make_unique<PreparedBoard>();
    prepared->board = board;
    std::ifstream encoded_file(dataset / "certificates" / (board.id + ".m3cert"), std::ios::binary);
    require(static_cast<bool>(encoded_file), "can't read certificate " + board.id);
    const std::string encoded{std::istreambuf_iterator<char>(encoded_file), {}};
    const auto problem = read_problem(dataset / "boards" / (board.id + ".txt"), board.goal, board.moves, wave_size_cap);
    require(strict::verify_certificate(problem, encoded, wave_count_cap), "source certificate verification failed");
    std::istringstream buffer(encoded);
    auto [rules, cert] = m3::read_certificate(buffer);
    require(rules.refill_cap == wave_size_cap && rules.cascade_budget == wave_count_cap, "incompatible source rules");
    prepared->rules = rules;
    prepared->certificate = std::move(cert);
    const strict::SearchBudget proof_budget{16'000, 45'000, 600, wave_count_cap};
    prepared->oracle = std::make_unique<Oracle>(prepared->certificate, rules, proof_budget, offline_solve_limit);
    build_bank(*prepared, study_seed);
    prepared->offline_cpu_ns = thread_cpu_ns() - cpu_before;
    prepared->offline_wall_ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - before).count());
    return prepared;
}

struct ControllerSpec {
    std::string_view name;
    int k;
};

std::vector<ControllerSpec> controller_specs(std::string_view selection)
{
    if (selection == "adaptive") {
        return {{"Adaptive", -1}};
    }
    if (selection == "pair") {
        return {{"Witness", 0}, {"Adaptive", -1}};
    }
    require(selection == "all", "controller selection must be all, pair or adaptive");
    return {{"Witness", 0}, {"Fixed1", 1}, {"Fixed3", 3}, {"Fixed6", 6}, {"Adaptive", -1}};
}

double trace_mass(const std::vector<std::vector<std::uint8_t>>& waves, int colors)
{
    double log_mass = 0;
    for (const auto& wave : waves) {
        const auto quota = m3::balanced_quota(colors, static_cast<int>(wave.size()));
        log_mass -= std::lgamma(static_cast<double>(wave.size() + 1));
        for (int count : quota) {
            log_mass += std::lgamma(static_cast<double>(count + 1));
        }
    }
    return std::exp(log_mass);
}

std::string wave_text(const std::vector<std::uint8_t>& wave)
{
    std::string text;
    for (const auto color : wave) {
        require(color < 10, "digit-wave encoding requires at most ten colors");
        text += static_cast<char>('0' + color);
    }
    return text;
}

struct AcrCounts {
    Counts aggregate;
    int candidate_checks{}, refill_nodes{}, states{}, timeout_fallbacks{}, cap_fallbacks{}, planned_witness_turns{};
};

void play_acr(const PreparedBoard& prepared, AgentChooser& chooser, ControllerSpec controller, int agent,
              int episode, std::uint64_t sample_seed, LookupCache* cache, AcrCounts& acr_counts,
              std::ostream& episodes, std::ostream& turns)
{
    const auto& board = prepared.board;
    auto& counts = acr_counts.aggregate;
    auto current = prepared.oracle->root();
    Trial trial;
    while (trial.turns < board.moves) {
        const auto& state = current.node().state;
        const auto usual = mix(sample_seed ^ board.seed ^ mix(static_cast<std::uint64_t>(episode) + 0x252a8174ULL)
                               ^ mix(static_cast<std::uint64_t>(trial.turns) + 0xec8bb24bULL));
        const auto& witness = chooser.choose(current.node(), agent, usual);
        const auto& bucket = prepared.buckets.at(bucket_index(current, witness));
        const int k = controller.k < 0 ? adaptive_k(prepared, current, witness) : controller.k;
        auto choice = choose_refill(*prepared.oracle, current, witness, k, usual, cache);
        const auto planned_witness = controller.k < 0 && k == 0;
        const auto relied_on_witness = choice.fallback || planned_witness;
        const auto& step = choice.step;
        require(step.waves > 0 && step.waves <= wave_count_cap &&
                    static_cast<int>(step.wave_sizes.size()) == step.waves &&
                    std::accumulate(step.wave_sizes.begin(), step.wave_sizes.end(), 0) == step.clear &&
                    step.goal == std::max(0, state.goal - step.clear) && step.moves == state.moves - 1,
                "invalid executed controlled turn");
        const auto witnessed = trace_mass(witness.refills, state.board.colors);
        const auto forecast_fallback = std::pow(1.0 - bucket.estimate.z_lower, k);
        turns << board.id << ',' << agents[agent] << ',' << controller.name << ',' << episode << ',' << trial.turns + 1
              << ',' << state.goal << ',' << state.moves << ',' << witness.swap.a << ',' << witness.swap.b << ','
              << bucket_names.at(bucket_index(current, witness)) << ',' << bucket.estimate.z_point << ','
              << bucket.estimate.z_lower << ',' << bucket.estimate.z_possible_upper << ',' << witnessed << ',' << k << ','
              << forecast_fallback << ',' << choice.candidate_checks << ',' << choice.refill_nodes << ',' << choice.states
              << ',' << choice.safe << ',' << choice.unsafe << ',' << choice.unresolved << ',' << choice.cache_hits
              << ',' << choice.used_witness << ',' << relied_on_witness << ',' << choice.timeout << ',' << choice.capped
              << ',' << choice.same_witness << ',' << choice.cpu_ns << ',' << choice.wall_ns << ',' << step.clear << ','
              << step.waves << ',' << wave_text(choice.refills.front()) << ',' << wave_text(witness.refills.front()) << ','
              << planned_witness << '\n';
        counts.witness += choice.used_witness;
        counts.natural += !choice.used_witness;
        counts.fallback += relied_on_witness;
        counts.proposals += choice.candidate_checks;
        counts.safe += choice.safe;
        counts.unsafe += choice.unsafe;
        counts.unknown += choice.unresolved;
        counts.cache_hits += choice.cache_hits;
        counts.same_witness += choice.same_witness;
        acr_counts.candidate_checks += choice.candidate_checks;
        acr_counts.refill_nodes += choice.refill_nodes;
        acr_counts.states += choice.states;
        acr_counts.timeout_fallbacks += choice.timeout;
        acr_counts.cap_fallbacks += choice.capped;
        acr_counts.planned_witness_turns += planned_witness;
        ++trial.turns;
        trial.clears += step.clear;
        trial.productive += std::clamp(state.goal - step.wave_sizes.front(), 0, step.extra);
        trial.extra_waves += step.waves - 1;
        trial.max_waves = std::max(trial.max_waves, step.waves);
        trial.proposals += choice.candidate_checks;
        trial.fallbacks += relied_on_witness;
        if (step.goal == 0) {
            trial.left = step.moves;
            ++counts.wins;
            break;
        }
        require(step.continuation.certificate && step.moves > 0 && step.continuation.node().state.goal == step.goal &&
                    step.continuation.node().state.moves == step.moves,
                "executed nonterminal successor lacks a matching verified continuation");
        current = step.continuation;
    }
    require(trial.clears >= board.goal && trial.productive <= board.goal, "episode missed Strict target");
    ++counts.episodes;
    counts.early += trial.left > 0;
    counts.challenge += trial.clears >= board.goal + challenge_extra;
    counts.chain += trial.extra_waves > 0;
    counts.productive_extra += trial.productive;
    counts.clears += trial.clears;
    counts.turns += trial.turns;
    counts.extra_waves += trial.extra_waves;
    counts.max_waves = std::max(counts.max_waves, trial.max_waves);
    episodes << board.id << ',' << agents[agent] << ',' << controller.name << ',' << episode << ',' << trial.turns
             << ',' << trial.clears << ',' << trial.left << ',' << (trial.extra_waves > 0) << ',' << trial.productive
             << ',' << trial.extra_waves << ',' << trial.max_waves << ',' << trial.proposals << ',' << trial.fallbacks << '\n';
}

struct AcrBoardOutput {
    std::string summary, episodes, turns, mass, references, offline;
};

AcrBoardOutput evaluate_acr_board(const fs::path& dataset, const BoardSpec& board, int runs,
                                  std::uint64_t sample_seed, bool warm, const std::vector<ControllerSpec>& controllers)
{
    const auto prepared = prepare_board(dataset, board, sample_seed);
    const auto& proof = prepared->certificate;
    AgentChooser chooser(proof, prepared->rules, board.seed);
    std::ostringstream summary, episodes, turns, mass, references, offline;
    turns << std::setprecision(15);
    mass << std::setprecision(15);
    references << std::setprecision(15);
    for (std::size_t b = 0; b < bucket_names.size(); ++b) {
        const auto& row = prepared->buckets.at(b);
        mass << board.id << ',' << board.recipe << ',' << bucket_names.at(b) << ',' << row.slots << ','
             << row.counts.safe << ',' << row.counts.unsafe << ',' << row.counts.unresolved << ','
             << row.estimate.samples << ',' << row.estimate.z_point << ',' << row.estimate.z_lower << ','
             << row.estimate.z_possible_upper << ',' << acr::mass_source_name(row.estimate.source) << ',' << row.borrowed
             << ',' << row.k << ',' << row.forecast_fallback << ',' << row.forecast_objective << '\n';
    }
    std::vector<LookupCache> caches(controllers.size());
    for (int agent = 0; agent < static_cast<int>(agents.size()); ++agent) {
        auto& policy = chooser.original_policy(agent);
        const auto [challenge, early, chain] = policy.exact(challenge_extra);
        const auto paired = exact_comparator_witness(proof, chooser, agent);
        references << board.id << ',' << agents[agent] << ',' << early << ',' << challenge << ',' << chain << ','
                   << exact_productive(proof, policy) << ',' << paired.early << ',' << paired.challenge << ','
                   << paired.chain << ',' << paired.productive << '\n';
        for (std::size_t index = 0; index < controllers.size(); ++index) {
            const auto controller = controllers.at(index);
            AcrCounts acr_counts;
            for (int run = 0; run < runs; ++run) {
                play_acr(*prepared, chooser, controller, agent, run, sample_seed,
                         warm ? &caches.at(index) : nullptr, acr_counts, episodes, turns);
            }
            const auto& c = acr_counts.aggregate;
            require(c.episodes == runs && c.wins == runs && c.turns == c.witness + c.natural &&
                        c.proposals == c.safe + c.unsafe + c.unknown && c.safe >= c.natural,
                    "controller counts mismatch");
            summary << board.id << ',' << board.seed << ',' << board.recipe << ',' << board.goal << ',' << board.moves
                    << ',' << agents[agent] << ',' << controller.name << ',' << c.episodes << ',' << c.wins << ',' << c.early
                    << ',' << c.challenge << ',' << c.chain << ',' << c.productive_extra << ',' << c.clears << ',' << c.turns
                    << ',' << c.witness << ',' << c.natural << ',' << c.fallback << ',' << c.proposals << ',' << c.safe << ','
                    << c.unsafe << ',' << c.unknown << ',' << c.same_witness << ',' << acr_counts.candidate_checks << ','
                    << acr_counts.refill_nodes << ',' << acr_counts.states << ',' << c.cache_hits << ','
                    << acr_counts.timeout_fallbacks << ',' << acr_counts.cap_fallbacks << ',' << c.extra_waves << ','
                    << c.max_waves << ',' << acr_counts.planned_witness_turns << '\n';
        }
    }
    const auto& stats = prepared->oracle->stats();
    offline << board.id << ',' << board.recipe << ',' << prepared->offline_cpu_ns << ',' << prepared->offline_wall_ns << ','
            << stats.proposals << ',' << stats.solver_calls << ',' << stats.certificates_verified << ','
            << stats.natural_edges_replayed << ',' << stats.retained_certificate_nodes << ',' << stats.candidate_checks
            << ',' << stats.refill_nodes << ',' << stats.states;
    for (const auto outcome : stats.outcomes) {
        offline << ',' << outcome;
    }
    offline << '\n';
    return {summary.str(), episodes.str(), turns.str(), mass.str(), references.str(), offline.str()};
}

} // namespace

int main(int argc, char** argv) try {
    require(argc >= 7 && argc <= 9,
            "usage: acr-benchmark DATASET OUTPUT EPISODES PER_RECIPE WORKERS SEED [warm|cold] [all|pair|adaptive]");
    const fs::path dataset = argv[1];
    const fs::path output = argv[2];
    const int runs = std::stoi(argv[3]);
    const int quota = std::stoi(argv[4]);
    const int workers = std::stoi(argv[5]);
    const auto seed = std::stoull(argv[6], nullptr, 0);
    const std::string_view cache = argc >= 8 ? argv[7] : "warm";
    const auto controllers = controller_specs(argc >= 9 ? argv[8] : "all");
    require(runs > 0 && quota > 0 && workers > 0 && (cache == "warm" || cache == "cold"), "invalid benchmark options");
    require(!fs::exists(output), "output already exists");
    const auto boards = read_boards(dataset, quota);
    const auto started = std::chrono::steady_clock::now();
    std::vector<AcrBoardOutput> results(boards.size());
    std::atomic_size_t next{}, complete{};
    std::vector<std::future<void>> jobs;
    for (int worker = 0; worker < workers; ++worker) {
        jobs.push_back(std::async(std::launch::async, [&] {
            for (auto index = next++; index < boards.size(); index = next++) {
                try {
                    results[index] = evaluate_acr_board(dataset, boards[index], runs, seed, cache == "warm", controllers);
                } catch (const std::exception& error) {
                    throw std::runtime_error(boards[index].id + ": " + error.what());
                }
                const auto done = ++complete;
                if (done % 10 == 0 || done == boards.size()) {
                    std::cerr << "simulated " << done << '/' << boards.size() << " boards\n";
                }
            }
        }));
    }
    for (auto& job : jobs) {
        job.get();
    }
    fs::create_directories(output);
    std::ofstream summary(output / "summary.csv"), episodes(output / "episodes.csv"), turns(output / "turns.csv"),
        mass(output / "mass.csv"), references(output / "witness-exact.csv"), offline(output / "offline-bank.csv");
    require(summary && episodes && turns && mass && references && offline, "cannot open ACR output tables");
    summary << "id,seed,recipe,goal,moves,agent,controller,episodes,wins,early,challenge,chain,productive_extra,clears,"
               "total_turns,executed_witness,executed_proposal,witness_fallbacks,proposal_attempts,proposal_safe,"
               "proposal_unsafe,proposal_unknown,proposal_same_witness,candidate_checks,refill_nodes,states,"
               "cache_hits,timeout_fallbacks,cap_fallbacks,extra_waves,max_waves,planned_witness_turns\n";
    episodes << "id,agent,controller,run,turns,clears,left,chain,productive_extra,extra_waves,max_waves,proposals,fallbacks\n";
    turns << "id,agent,controller,run,turn,remaining_goal,remaining_moves,swap_a,swap_b,bucket,z_point,z_lower,z_upper,witness_p0,"
             "k,forecast_fallback,candidate_checks,refill_nodes,states,proposal_safe,proposal_unsafe,proposal_unknown,cache_hits,"
             "used_witness,fallback,timeout,capped,same_witness,cpu_ns,wall_ns,clear,waves,first_wave,first_wave_witness,planned_witness\n";
    mass << "id,recipe,bucket,slots,safe,unsafe,unresolved,forecast_samples,z_point,z_lower,z_upper,source,borrowed,k,"
            "forecast_fallback,forecast_objective\n";
    references << "id,agent,exact_early,exact_challenge,exact_chain,exact_mean_productive_extra_tiles,"
                  "paired_exact_early,paired_exact_challenge,paired_exact_chain,paired_exact_mean_productive_extra_tiles\n";
    offline << "id,recipe,cpu_ns,wall_ns,proposals,solver_calls,certificates_verified,natural_edges_replayed,"
               "retained_certificate_nodes,candidate_checks,refill_nodes,states";
    for (const auto name : refill_study::outcome_names) {
        offline << ',' << name;
    }
    offline << '\n';
    for (const auto& result : results) {
        summary << result.summary;
        episodes << result.episodes;
        turns << result.turns;
        mass << result.mass;
        references << result.references;
        offline << result.offline;
    }
    summary.close(); episodes.close(); turns.close(); mass.close(); references.close(); offline.close();
    require(summary && episodes && turns && mass && references && offline, "failed writing ACR tables");
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::cerr << "completed " << boards.size() << " boards in " << seconds << " s, cache=" << cache << '\n';
    return 0;
} catch (const std::exception& error) {
    std::cerr << "ACR benchmark failed: " << error.what() << '\n';
    return 1;
}
