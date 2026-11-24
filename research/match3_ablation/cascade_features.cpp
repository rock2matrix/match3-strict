#define main previous_simulator_main
#include "simulate_for_cascade.inc"
#undef main

#include <chrono>
#include <set>

namespace {

// The last bin contains all outcomes with at least nine extra-wave clears.
constexpr int last_bin = 9;
constexpr double tolerance = 1e-10;

struct Features {
    std::array<double, last_bin + 1> distribution{};
    double extra_tiles = 0;
    double productive_extra_tiles = 0;
    double all_tiles = 0;
    double early_and_challenge = 0;
};

struct EdgeTiles {
    int all;
    int extra;
    int productive;
};

EdgeTiles count_edge_tiles(const m3::Edge& edge, int remaining_goal)
{
    const auto first_wave = static_cast<int>(edge.refills.front().size());
    const auto all_waves = edge_clears(edge);
    const auto extra = all_waves - first_wave;
    const auto productive = std::clamp(remaining_goal - first_wave, 0, extra);
    return {all_waves, extra, productive};
}

double at_least(const Features& features, int threshold)
{
    return std::accumulate(features.distribution.begin() + threshold, features.distribution.end(), 0.0);
}

double capped_share(const Features& features, int cap)
{
    double value = 0;
    for (int bin = 1; bin <= last_bin; ++bin) {
        value += features.distribution[bin] * std::min(bin, cap) / cap;
    }
    return value;
}

class CascadeEvaluator {
public:
    CascadeEvaluator(const m3::Certificate& cert, Policy& policy)
        : cert_(cert), policy_(policy), cache_(cert.nodes.size())
    {
    }

    const Features& from(std::size_t index)
    {
        auto& entry = cache_[index];
        if (entry) {
            return *entry;
        }
        auto& result = entry.emplace();
        const auto& node = cert_.nodes[index];
        if (node.state.goal == 0) {
            result.distribution[0] = 1;
            return result;
        }

        const auto& chosen = policy_.cascade_choices(index).edges;
        const double probability = 1.0 / chosen.size();
        for (const auto index_in_node : chosen) {
            const auto& edge = node.edges[index_in_node];
            const auto counts = count_edge_tiles(edge, node.state.goal);
            const auto& downstream = from(edge.child);
            result.extra_tiles += probability * (downstream.extra_tiles + counts.extra);
            result.productive_extra_tiles += probability * (downstream.productive_extra_tiles + counts.productive);
            result.all_tiles += probability * (downstream.all_tiles + counts.all);
            // Before a terminal edge, earlier clears are exactly root goal minus
            // this state's remaining goal. Only the terminating edge can overshoot.
            const auto& child = cert_.nodes[edge.child].state;
            const bool finishes_early_with_surplus = child.goal == 0 && child.moves > 0 &&
                                                     counts.all >= node.state.goal + 3;
            result.early_and_challenge += probability * (downstream.early_and_challenge + finishes_early_with_surplus);
            for (int bin = 0; bin <= last_bin; ++bin) {
                const int total = std::min(bin + counts.extra, last_bin);
                result.distribution[total] += probability * downstream.distribution[bin];
            }
        }
        return result;
    }

private:
    const m3::Certificate& cert_;
    Policy& policy_;
    std::vector<std::optional<Features>> cache_;
};

// Enumerate complete trajectories for one level per recipe, without sharing
// partial path totals or distributions across transposed certificate states.
void enumerate_games(const m3::Certificate& cert, Policy& policy, std::size_t index,
                     double probability, EdgeTiles prior, Features& result)
{
    const auto& node = cert.nodes[index];
    if (node.state.goal == 0) {
        result.distribution[std::min(prior.extra, last_bin)] += probability;
        result.extra_tiles += probability * prior.extra;
        result.productive_extra_tiles += probability * prior.productive;
        result.all_tiles += probability * prior.all;
        if (node.state.moves > 0 && prior.all >= cert.nodes[cert.root].state.goal + 3) {
            result.early_and_challenge += probability;
        }
        return;
    }
    const auto& choices = policy.cascade_choices(index).edges;
    for (const auto edge_index : choices) {
        const auto& edge = node.edges[edge_index];
        auto next = prior;
        int still_needed = node.state.goal;
        for (std::size_t wave = 0; wave < edge.refills.size(); ++wave) {
            const auto size = static_cast<int>(edge.refills[wave].size());
            next.all += size;
            if (wave > 0) {
                next.extra += size;
                next.productive += std::min(size, still_needed);
            }
            still_needed = std::max(0, still_needed - size);
        }
        enumerate_games(cert, policy, edge.child, probability / choices.size(), next, result);
    }
}

void check_features(const m3::Certificate& cert, Policy& policy, const Features& features,
                    double chain, double early, double challenge, bool cross_check)
{
    require(std::abs(at_least(features, 0) - 1) < tolerance, "terminal distribution does not sum to one");
    require(std::abs(at_least(features, 3) - chain) < tolerance, "positive cascade tiles must agree with V1 chain event");
    require(features.extra_tiles + tolerance >= 3 * chain, "expectation below the smallest nonzero cascade event");
    require(features.extra_tiles + tolerance >= features.productive_extra_tiles &&
                features.all_tiles + tolerance >= features.extra_tiles,
            "cascade tile subtotals do not conserve clears");
    require(features.early_and_challenge + tolerance >= std::max(0.0, early + challenge - 1) &&
                features.early_and_challenge <= std::min(early, challenge) + tolerance,
            "joint challenge event is inconsistent with its marginals");
    if (!cross_check) {
        return;
    }
    Features enumerated;
    enumerate_games(cert, policy, cert.root, 1, {0, 0, 0}, enumerated);
    require(std::abs(enumerated.extra_tiles - features.extra_tiles) < tolerance &&
                std::abs(enumerated.productive_extra_tiles - features.productive_extra_tiles) < tolerance &&
                std::abs(enumerated.all_tiles - features.all_tiles) < tolerance &&
                std::abs(enumerated.early_and_challenge - features.early_and_challenge) < tolerance,
            "exact counts disagree with explicit path enumeration");
    for (std::size_t bin = 0; bin < enumerated.distribution.size(); ++bin) {
        require(std::abs(enumerated.distribution[bin] - features.distribution[bin]) < tolerance,
                "exact cascade distribution disagrees with explicit path enumeration");
    }
}

} // namespace

int main(int argc, char** argv) try {
    require(argc == 3, "usage: cascade-features DATASET_DIR OUTPUT_CSV");
    const fs::path dataset = argv[1];
    std::ifstream candidates(dataset / "candidates.csv");
    require(static_cast<bool>(candidates), "cannot read candidates.csv");
    std::string line;
    std::getline(candidates, line);
    std::unordered_map<std::string, std::size_t> column;
    for (const auto& field : csv_fields(line)) {
        column.emplace(field, column.size());
    }

    const fs::path destination = argv[2];
    auto temporary = destination;
    temporary += ".tmp";
    std::ofstream output(temporary);
    require(static_cast<bool>(output), "cannot create exact cascade table");
    output << "id,agent,goal,moves,exact_early,exact_challenge,exact_chain,exact_mean_extra_tiles,"
              "exact_extra_ge3,exact_extra_ge6,exact_extra_ge9,exact_mean_productive_extra_tiles,"
              "exact_mean_clears,exact_extra_tiles_capped6,exact_extra_tiles_capped9,"
              "exact_early_and_challenge,verify_us,score_us\n" << std::setprecision(15);

    using Clock = std::chrono::steady_clock;
    std::unordered_set<std::string> seen;
    std::set<std::pair<int, int>> checked_recipes;
    while (std::getline(candidates, line)) {
        const auto fields = csv_fields(line);
        const auto field = [&](const std::string& key) -> const std::string& {
            return fields.at(column.at(key));
        };
        const bool certified = field("verdict") == "WIN_CERTIFIED";
        require(certified == (field("cert_verified") == "1"), "certification columns disagree: " + field("id"));
        if (!certified) {
            continue;
        }
        const auto& id = field("id");
        require(seen.insert(id).second, "duplicate certified ID: " + id);
        const int goal = std::stoi(field("goal"));
        const int moves = std::stoi(field("moves"));
        const auto seed = std::stoull(field("seed"));
        std::ifstream file(dataset / "certificates" / (id + ".m3cert"), std::ios::binary);
        require(static_cast<bool>(file), "cannot read certificate for " + id);
        const std::string certificate{std::istreambuf_iterator<char>(file), {}};
        std::istringstream encoded(certificate);
        std::string version;
        int schema = 0;
        int cap = 0;
        int depth = 0;
        require(static_cast<bool>(encoded >> version >> schema >> cap >> depth), "invalid certificate header");
        require(cap == 12 && depth == 9 && goal > 0 && moves > 0, "unexpected experimental rules: " + id);
        const auto problem = read_problem(dataset / "boards" / (id + ".txt"), goal, moves, cap);
        const auto verify_start = Clock::now();
        require(strict::verify_certificate(problem, certificate, depth), "independent certificate replay failed: " + id);
        const auto verify_us = std::chrono::duration<double, std::micro>(Clock::now() - verify_start).count();
        encoded.seekg(0);
        const auto [rules, cert] = m3::read_certificate(encoded);
        const bool cross_check = checked_recipes.emplace(goal, moves).second;

        for (int agent = 0; agent < static_cast<int>(agents.size()); ++agent) {
            const auto score_start = Clock::now();
            Policy policy(cert, rules, seed, agent);
            const auto [challenge, early, chain] = policy.exact(3);
            CascadeEvaluator evaluator(cert, policy);
            const auto& features = evaluator.from(cert.root);
            const auto score_us = std::chrono::duration<double, std::micro>(Clock::now() - score_start).count();
            check_features(cert, policy, features, chain, early, challenge, cross_check);
            output << id << ',' << agents[agent] << ',' << goal << ',' << moves << ',' << early << ','
                   << challenge << ',' << chain << ',' << features.extra_tiles << ',' << at_least(features, 3)
                   << ',' << at_least(features, 6) << ',' << at_least(features, 9) << ','
                   << features.productive_extra_tiles << ',' << features.all_tiles << ',' << capped_share(features, 6)
                   << ',' << capped_share(features, 9) << ',' << features.early_and_challenge << ','
                   << verify_us << ',' << score_us << '\n';
        }
        if (seen.size() % 25 == 0) {
            std::cerr << "scored " << seen.size() << " certified boards\n";
        }
    }
    output.close();
    require(!seen.empty() && output, "empty or incomplete exact cascade results");
    fs::rename(temporary, destination);
    std::cerr << "scored " << seen.size() << " certified boards; enumerated all agent paths on "
              << checked_recipes.size() << " boards\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "Exact cascade extraction failed: " << error.what() << '\n';
    return 1;
}
