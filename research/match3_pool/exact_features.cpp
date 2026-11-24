#define main prior_experiment_main
#include "simulate_for_pool.inc"
#undef main

#include <chrono>

namespace {

struct TerminalBucket {
    double probability = 0;
    double cleared_weighted = 0;
    double extra_waves_weighted = 0;
    double no_chain_probability = 0;
};

struct ExactFeatures {
    double mean_moves_left = 0;
    double mean_surplus = 0;
    double chain_probability = 0;
    double mean_extra_waves = 0;
    double mean_turns = 0;
    double mean_extra_waves_per_turn = 0;
};

class ExactFeatureEvaluator {
public:
    ExactFeatureEvaluator(const m3::Certificate& cert, Policy& policy)
        : cert_(cert), policy_(policy), cache_(cert.nodes.size())
    {
    }

    ExactFeatures evaluate()
    {
        const auto& root = cert_.nodes[cert_.root].state;
        const auto& buckets = from(cert_.root);
        ExactFeatures result;
        double mass = 0;
        double mass_without_chain = 0;
        for (int left = 0; left < root.moves; ++left) {
            const auto& bucket = buckets[left];
            const int used = root.moves - left;
            mass += bucket.probability;
            mass_without_chain += bucket.no_chain_probability;
            result.mean_moves_left += left * bucket.probability;
            result.mean_turns += used * bucket.probability;
            result.mean_surplus += bucket.cleared_weighted - root.goal * bucket.probability;
            result.mean_extra_waves += bucket.extra_waves_weighted;
            result.mean_extra_waves_per_turn += bucket.extra_waves_weighted / used;
        }
        require(std::abs(mass - 1) < 1e-10, "policy does not reach a terminal state with unit probability");
        require(result.mean_surplus > -1e-10, "certified policy misses the clear goal");
        result.mean_surplus = std::max(0.0, result.mean_surplus);
        result.chain_probability = std::clamp(1 - mass_without_chain, 0.0, 1.0);
        return result;
    }

private:
    const std::vector<TerminalBucket>& from(std::size_t index)
    {
        auto& entry = cache_[index];
        if (!entry.empty()) {
            return entry;
        }
        const auto& node = cert_.nodes[index];
        entry.resize(node.state.moves + 1);
        if (node.state.goal == 0) {
            auto& terminal = entry[node.state.moves];
            terminal.probability = 1;
            terminal.no_chain_probability = 1;
            return entry;
        }

        const auto& edges = policy_.pool_choices(index).edges;
        const double p = 1.0 / edges.size();
        for (auto edge_index : edges) {
            const auto& edge = node.edges[edge_index];
            const auto& downstream = from(edge.child);
            const int clears = edge_clears(edge);
            const auto extra = edge.refills.size() - 1;
            for (std::size_t left = 0; left < downstream.size(); ++left) {
                const auto& next = downstream[left];
                auto& here = entry[left];
                here.probability += p * next.probability;
                here.cleared_weighted += p * (next.cleared_weighted + clears * next.probability);
                here.extra_waves_weighted += p * (next.extra_waves_weighted + extra * next.probability);
                if (extra == 0) {
                    here.no_chain_probability += p * next.no_chain_probability;
                }
            }
        }
        return entry;
    }

    const m3::Certificate& cert_;
    Policy& policy_;
    std::vector<std::vector<TerminalBucket>> cache_;
};

} // namespace

int main(int argc, char** argv) try {
    require(argc == 3 || (argc == 4 && std::string_view(argv[3]) == "random-minimal"),
            "usage: exact-features DATASET_DIR OUTPUT_CSV [random-minimal]");
    const bool random_minimal = argc == 4;
    const fs::path dataset = argv[1];
    std::ifstream candidates(dataset / "candidates.csv");
    require(static_cast<bool>(candidates), "cannot read candidates.csv");
    std::string line;
    std::getline(candidates, line);
    std::unordered_map<std::string, std::size_t> column;
    for (const auto& field : csv_fields(line)) {
        column.emplace(field, column.size());
    }

    const fs::path output_path = argv[2];
    auto temp_path = output_path;
    temp_path += ".tmp";
    std::ofstream output(temp_path);
    require(static_cast<bool>(output), "cannot create exact feature CSV");
    output << "id,agent,goal,moves,exact_early,exact_challenge,exact_chain,verify_us,score_us";
    if (!random_minimal) {
        output << ",exact_mean_moves_left,exact_mean_surplus,exact_mean_extra_waves,exact_turns,"
                  "exact_mean_extra_waves_per_turn";
    }
    output << '\n' << std::setprecision(15);

    std::unordered_set<std::string> seen;
    while (std::getline(candidates, line)) {
        const auto fields = csv_fields(line);
        const auto field = [&](const std::string& key) -> const std::string& {
            return fields.at(column.at(key));
        };
        if (field("verdict") != "WIN_CERTIFIED" || field("cert_verified") != "1") {
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
        using Clock = std::chrono::steady_clock;
        const auto verify_start = Clock::now();
        require(strict::verify_certificate(problem, certificate, depth), "independent certificate replay failed: " + id);
        const auto verify_us = std::chrono::duration<double, std::micro>(Clock::now() - verify_start).count();
        encoded.seekg(0);
        const auto [rules, cert] = m3::read_certificate(encoded);

        const int agent_count = random_minimal ? 1 : static_cast<int>(agents.size());
        for (int agent = 0; agent < agent_count; ++agent) {
            const auto score_start = Clock::now();
            Policy policy(cert, rules, seed, agent);
            const auto [challenge, early, chain] = policy.exact(3);
            ExactFeatures result;
            if (!random_minimal) {
                ExactFeatureEvaluator evaluator(cert, policy);
                result = evaluator.evaluate();
                require(std::abs(chain - result.chain_probability) < 1e-10, "chain calculation disagrees");
            }
            const auto score_us = std::chrono::duration<double, std::micro>(Clock::now() - score_start).count();
            output << id << ',' << agents[agent] << ',' << goal << ',' << moves << ','
                   << early << ',' << challenge << ',' << chain << ',' << verify_us << ',' << score_us;
            if (!random_minimal) {
                output << ',' << result.mean_moves_left << ',' << result.mean_surplus << ','
                       << result.mean_extra_waves << ',' << result.mean_turns << ','
                       << result.mean_extra_waves_per_turn;
            }
            output << '\n';
        }
        if (seen.size() % 25 == 0) {
            std::cerr << "scored " << seen.size() << " certified boards\n";
        }
    }
    output.close();
    require(!seen.empty() && output, "empty or incomplete exact feature table");
    fs::rename(temp_path, output_path);
    std::cerr << "scored " << seen.size() << " certified boards total\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "Exact feature extraction failed: " << error.what() << '\n';
    return 1;
}
