// Reuse the simulator's quota-conforming natural turn and shortcut classifier.
#define main simulation_main
#include "simulate.cpp"
#undef main

#include <atomic>
#include <future>
#include <map>

namespace {

constexpr int audit_draws = 128;
constexpr strict::SearchBudget audit_budget{16'000, 45'000, 600, 9};
constexpr std::uint64_t audit_seed = 0x1fe21d315cb4a976ULL;

struct AuditLevel {
    std::string id;
    std::string stratum;
    int goal;
    int moves;
    std::uint64_t seed;
};

struct AuditCounts {
    int samples = 0;
    int shortcut_safe = 0;
    int shortcut_unsafe = 0;
    int solver_safe = 0;
    int solver_unsafe = 0;
    int unresolved = 0;
    int depth_stops = 0;
    int cap_stops = 0;
    int solver_calls = 0;
    int cache_hits = 0;
};

void add_counts(AuditCounts& sum, const AuditCounts& counts)
{
    sum.samples += counts.samples;
    sum.shortcut_safe += counts.shortcut_safe;
    sum.shortcut_unsafe += counts.shortcut_unsafe;
    sum.solver_safe += counts.solver_safe;
    sum.solver_unsafe += counts.solver_unsafe;
    sum.unresolved += counts.unresolved;
    sum.depth_stops += counts.depth_stops;
    sum.cap_stops += counts.cap_stops;
    sum.solver_calls += counts.solver_calls;
    sum.cache_hits += counts.cache_hits;
}

Safety probe_successor(const Turn& turn, const m3::State& source, const m3::Rules& rules)
{
    const auto& board = turn.board;
    const strict::Problem problem{
        {board.rows, board.cols, board.colors, board.cells},
        source.goal - turn.cleared, source.moves - 1, {rules.refill_cap}
    };
    const auto result = strict::solve(problem, audit_budget);
    if (result.verdict == strict::Verdict::win_certified) {
        require(strict::verify_certificate(problem, result.certificate, audit_budget.cascade_limit),
                "newly certified safe successor failed replay");
        return Safety::known_safe;
    }
    return result.verdict == strict::Verdict::fail_certified ? Safety::known_unsafe : Safety::unresolved;
}

AuditCounts audit_level(const fs::path& dataset, const AuditLevel& level)
{
    std::ifstream file(dataset / "certificates" / (level.id + ".m3cert"), std::ios::binary);
    require(static_cast<bool>(file), "cannot read certificate for " + level.id);
    const std::string text{std::istreambuf_iterator<char>(file), {}};
    std::istringstream encoded(text);
    const auto [rules, cert] = m3::read_certificate(encoded);
    require(rules.refill_cap == 12 && rules.cascade_budget == audit_budget.cascade_limit,
            "unexpected rules: " + level.id);
    const auto problem = read_problem(dataset / "boards" / (level.id + ".txt"),
                                      level.goal, level.moves, rules.refill_cap);
    require(strict::verify_certificate(problem, text, rules.cascade_budget),
            "root certificate replay failed: " + level.id);

    std::unordered_set<std::string> verified_states;
    for (const auto& node : cert.nodes) {
        const auto& state = node.state;
        verified_states.insert(state_key(state.board, state.goal, state.moves));
    }
    const auto& root = cert.nodes.at(cert.root);
    const auto& state = root.state;
    require(!root.edges.empty() && state.goal > 0 && state.moves > 0, "unplayable board: " + level.id);

    // The fixed continuation search classifies each distinct stable child.
    // Repeated draws retain their original weight, including unresolved children.
    std::unordered_map<std::string, Safety> cache;
    std::mt19937_64 random(mix(level.seed ^ audit_seed));
    AuditCounts counts;
    for (int draw = 0; draw < audit_draws; ++draw) {
        const auto move = root.edges[m3::board_detail::uniform_below(random, root.edges.size())].swap;
        const auto turn = natural_turn(state.board, move, rules, random);
        ++counts.samples;
        counts.cap_stops += turn.stop == Stop::refill_cap;
        counts.depth_stops += turn.stop == Stop::depth;
        const auto shortcut = classify(turn, state, verified_states);
        if (shortcut != Safety::unresolved) {
            counts.shortcut_safe += shortcut == Safety::known_safe;
            counts.shortcut_unsafe += shortcut == Safety::known_unsafe;
            continue;
        }
        if (turn.stop == Stop::depth) {
            ++counts.unresolved;
            continue;
        }

        const auto key = state_key(turn.board, state.goal - turn.cleared, state.moves - 1);
        const auto found = cache.find(key);
        Safety result;
        if (found == cache.end()) {
            result = probe_successor(turn, state, rules);
            cache.emplace(key, result);
            ++counts.solver_calls;
        } else {
            result = found->second;
            ++counts.cache_hits;
        }
        counts.solver_safe += result == Safety::known_safe;
        counts.solver_unsafe += result == Safety::known_unsafe;
        counts.unresolved += result == Safety::unresolved;
    }
    return counts;
}

void write_counts(std::ostream& output, const AuditCounts& counts)
{
    const auto safe = counts.shortcut_safe + counts.solver_safe;
    const auto unsafe = counts.shortcut_unsafe + counts.solver_unsafe;
    require(safe + unsafe + counts.unresolved == counts.samples && counts.samples > 0,
            "audit counts do not reconcile");
    const auto n = static_cast<double>(counts.samples);
    const auto lower = safe / n;
    const auto upper = 1 - unsafe / n;
    // Two one-sided Hoeffding bounds, each with alpha=.025. Draws are independent
    // within this fixed set of boards, and need not share a common probability.
    // The resulting interval covers both endpoints jointly for each reported row.
    const auto radius = std::sqrt(std::log(40.0) / (2 * n));
    output << counts.samples << ',' << counts.shortcut_safe << ',' << counts.shortcut_unsafe << ','
           << counts.solver_safe << ',' << counts.solver_unsafe << ',' << counts.unresolved << ','
           << counts.depth_stops << ',' << counts.cap_stops << ',' << counts.solver_calls << ','
           << counts.cache_hits << ',' << safe << ',' << unsafe << ',' << lower << ',' << upper << ','
           << std::max(0.0, lower - radius) << ',' << std::min(1.0, upper + radius) << '\n';
}

std::vector<AuditLevel> load_levels(const fs::path& dataset)
{
    std::ifstream input(dataset / "candidates.csv");
    require(static_cast<bool>(input), "cannot read audit candidate list");
    std::string line;
    std::getline(input, line);
    std::unordered_map<std::string, std::size_t> columns;
    for (const auto& field : csv_fields(line)) {
        columns.emplace(field, columns.size());
    }
    std::vector<AuditLevel> levels;
    std::unordered_set<std::string> seen;
    while (std::getline(input, line)) {
        const auto values = csv_fields(line);
        const auto column = [&](const std::string& field) -> const std::string& {
            return values.at(columns.at(field));
        };
        const auto id = column("id");
        if (column("verdict") == "WIN_CERTIFIED" && column("cert_verified") == "1" && seen.insert(id).second) {
            levels.push_back({id, column("stratum"), std::stoi(column("goal")),
                              std::stoi(column("moves")), std::stoull(column("seed"))});
        }
    }
    return levels;
}

constexpr auto count_columns =
    "samples,shortcut_safe,shortcut_unsafe,solver_safe,solver_unsafe,unresolved,depth_stops,cap_stops,"
    "solver_calls,cache_hits,known_safe,known_unsafe,safe_mass_lower,safe_mass_upper,"
    "simultaneous_95_low,simultaneous_95_high";

} // namespace

int main(int argc, char** argv) try {
    require(argc == 4, "usage: audit DATASET_DIR OUTPUT_CSV WORKERS");
    const fs::path dataset = argv[1];
    const int workers = std::stoi(argv[3]);
    require(workers > 0, "worker count must be positive");
    const auto levels = load_levels(dataset);
    require(!levels.empty(), "no verified levels to audit");
    std::vector<AuditCounts> results(levels.size());
    std::atomic_size_t next = 0;
    std::vector<std::future<void>> tasks;
    for (int worker = 0; worker < workers; ++worker) {
        tasks.push_back(std::async(std::launch::async, [&] {
            for (auto index = next++; index < levels.size(); index = next++) {
                results[index] = audit_level(dataset, levels[index]);
            }
        }));
    }
    for (auto& task : tasks) {
        task.get();
    }

    std::ofstream output(argv[2]);
    require(static_cast<bool>(output), "cannot write audit table");
    output << "id,stratum,goal,moves," << count_columns << '\n' << std::setprecision(12);
    std::map<std::string, AuditCounts> groups;
    AuditCounts overall;
    for (std::size_t index = 0; index < levels.size(); ++index) {
        const auto& level = levels[index];
        const auto& counts = results[index];
        output << level.id << ',' << level.stratum << ',' << level.goal << ',' << level.moves << ',';
        write_counts(output, counts);
        add_counts(groups[level.stratum], counts);
        add_counts(overall, counts);
    }
    output.close();
    require(static_cast<bool>(output), "audit table write failed");
    std::cout << "stratum," << count_columns << '\n' << std::setprecision(12);
    for (const auto& [group, counts] : groups) {
        std::cout << group << ',';
        write_counts(std::cout, counts);
    }
    std::cout << "all,";
    write_counts(std::cout, overall);
    return 0;
} catch (const std::exception& error) {
    std::cerr << "Safety audit failed: " << error.what() << '\n';
    return 1;
}
