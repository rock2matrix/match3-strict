#include <match3_strict/solver.hpp>

#include "detail/verify.hpp"

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <random>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace {

namespace fs = std::filesystem;
namespace strict = match3_strict;

constexpr int rolls = 1000;
constexpr int natural_rolls = 512;
constexpr int root_probes = 256;
constexpr int policy_probes = 24;
constexpr int challenge_extra = 3;
constexpr std::array agents{"Random", "Greedy", "Objective", "Lookahead"};

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::uint64_t mix(std::uint64_t seed)
{
    seed = (seed ^ (seed >> 30)) * 0xbf58476d1ce4e5b9ULL;
    seed = (seed ^ (seed >> 27)) * 0x94d049bb133111ebULL;
    return seed ^ (seed >> 31);
}

enum class Stop { stable, refill_cap, depth };

struct Turn {
    m3::Board board;
    Stop stop = Stop::stable;
    int cleared = 0;
    int waves = 0;
    int impacted = 0;
};

Turn natural_turn(m3::Board board, m3::Swap swap, const m3::Rules& rules,
                  std::mt19937_64& random, bool track_impact = false)
{
    std::swap(board.cells[swap.a], board.cells[swap.b]);
    Turn result{std::move(board)};
    auto matches = m3::dirty_matches(result.board, {swap.a, swap.b});
    require(!matches.empty(), "natural run picked an illegal move");

    std::vector<bool> affected;
    if (track_impact) {
        affected.resize(result.board.cells.size());
        affected[swap.a] = affected[swap.b] = true;
    }
    while (!matches.empty()) {
        if (track_impact) {
            for (int cell : matches) {
                for (int index = cell; index >= 0; index -= result.board.cols) {
                    affected[index] = true;
                }
            }
        }
        if (matches.size() > static_cast<std::size_t>(rules.refill_cap)) {
            result.stop = Stop::refill_cap;
            break;
        }
        if (result.waves >= rules.cascade_budget) {
            result.stop = Stop::depth;
            break;
        }

        auto falling = m3::clear_and_drop(result.board, matches);
        const int holes = static_cast<int>(falling.holes.size());
        const auto quota = m3::balanced_quota(result.board.colors, holes);
        std::vector<std::uint8_t> batch;
        batch.reserve(holes);
        for (int color = 0; color < result.board.colors; ++color) {
            batch.insert(batch.end(), quota[color], static_cast<std::uint8_t>(color));
        }
        for (int index = holes - 1; index > 0; --index) {
            std::swap(batch[index], batch[m3::board_detail::uniform_below(random, index + 1)]);
        }
        m3::verification::apply_batch(falling.board, falling.holes, batch);
        result.cleared += holes;
        ++result.waves;
        result.board = std::move(falling.board);
        matches = m3::dirty_matches(result.board, falling.changed);
    }
    if (track_impact) {
        result.impacted = static_cast<int>(std::count(affected.begin(), affected.end(), true));
    }
    return result;
}

int immediate_clears(m3::Board board, m3::Swap swap)
{
    std::swap(board.cells[swap.a], board.cells[swap.b]);
    return static_cast<int>(m3::dirty_matches(board, {swap.a, swap.b}).size());
}

struct NaturalEpisodes {
    int cleared = 0;
    int swaps = 0;
    int goals = 0;
    int caps = 0;
    int depths = 0;
    int dead = 0;
};

NaturalEpisodes play_natural(const m3::State& root, const m3::Rules& rules, std::uint64_t seed)
{
    NaturalEpisodes results;
    std::mt19937_64 random(mix(seed ^ 0x31194647ULL));
    for (int episode = 0; episode < natural_rolls; ++episode) {
        auto state = root;
        while (state.moves > 0 && state.goal > 0) {
            const auto moves = m3::legal_swaps(state.board);
            if (moves.empty()) {
                ++results.dead;
                break;
            }
            const auto swap = moves[m3::board_detail::uniform_below(random, moves.size())];
            auto turn = natural_turn(std::move(state.board), swap, rules, random);
            results.cleared += turn.cleared;
            ++results.swaps;
            if (turn.stop == Stop::refill_cap) {
                ++results.caps;
                break;
            }
            if (turn.stop == Stop::depth) {
                ++results.depths;
                break;
            }
            state.goal -= turn.cleared;
            --state.moves;
            state.board = std::move(turn.board);
            if (state.goal <= 0) {
                ++results.goals;
            }
        }
    }
    return results;
}

std::string state_key(const m3::Board& board, int goal, int moves)
{
    std::string key(reinterpret_cast<const char*>(board.cells.data()), board.cells.size());
    key.append(reinterpret_cast<const char*>(&goal), sizeof(goal));
    key.append(reinterpret_cast<const char*>(&moves), sizeof(moves));
    return key;
}

enum class Safety { known_safe, known_unsafe, unresolved };

Safety classify(const Turn& turn, const m3::State& state,
                const std::unordered_set<std::string>& verified_states)
{
    if (turn.stop == Stop::refill_cap) {
        return Safety::known_unsafe;
    }
    if (turn.stop == Stop::depth) {
        return Safety::unresolved;
    }
    const int goal = state.goal - turn.cleared;
    if (goal <= 0 || verified_states.contains(state_key(turn.board, goal, state.moves - 1))) {
        return Safety::known_safe;
    }
    if (state.moves == 1 || m3::legal_swaps(turn.board).empty()) {
        return Safety::known_unsafe;
    }
    return Safety::unresolved;
}

double witness_mass(const m3::Edge& edge, int colors)
{
    double probability = 1;
    for (const auto& batch : edge.refills) {
        const int holes = static_cast<int>(batch.size());
        const auto quota = m3::balanced_quota(colors, holes);
        double log_probability = -std::lgamma(holes + 1.0);
        for (int count : quota) {
            log_probability += std::lgamma(count + 1.0);
        }
        probability *= std::exp(log_probability);
    }
    return probability;
}

struct RootFeatures {
    double entropy = 0;
    double gap = 0;
    double waves = 0;
    double wave_variance = 0;
    double impact = 0;
    double lower = 0;
    double upper = 0;
    double witness = 0;
    std::size_t samples = 0;
};

RootFeatures measure_root(const std::string& id, const m3::Certificate& cert,
                          const m3::Rules& rules, std::uint64_t seed, std::ostream& output)
{
    std::unordered_set<std::string> verified_states;
    for (const auto& node : cert.nodes) {
        const auto& state = node.state;
        verified_states.insert(state_key(state.board, state.goal, state.moves));
    }
    const auto& root = cert.nodes[cert.root];
    const auto& state = root.state;
    RootFeatures result;
    std::vector<double> values;
    double wave_squares = 0;
    int impacted = 0;
    int total_safe = 0;
    int total_unsafe = 0;
    for (const auto& edge : root.edges) {
        std::mt19937_64 random(mix(seed ^ 0x6b37a897ULL ^ (edge.swap.a * 257 + edge.swap.b)));
        int clears = 0;
        int waves = 0;
        int edge_impact = 0;
        int safe = 0;
        int unsafe = 0;
        int cap = 0;
        int depth = 0;
        for (int sample = 0; sample < root_probes; ++sample) {
            const auto turn = natural_turn(state.board, edge.swap, rules, random, true);
            clears += turn.cleared;
            waves += turn.waves;
            wave_squares += turn.waves * turn.waves;
            edge_impact += turn.impacted;
            const auto safety = classify(turn, state, verified_states);
            safe += safety == Safety::known_safe;
            unsafe += safety == Safety::known_unsafe;
            cap += turn.stop == Stop::refill_cap;
            depth += turn.stop == Stop::depth;
        }
        const double value = static_cast<double>(clears) / root_probes;
        const auto mass = witness_mass(edge, state.board.colors);
        values.push_back(value);
        result.waves += waves;
        impacted += edge_impact;
        total_safe += safe;
        total_unsafe += unsafe;
        result.witness += mass;
        result.samples += root_probes;
        output << id << ',' << edge.swap.a << ',' << edge.swap.b << ',' << root_probes << ','
               << immediate_clears(state.board, edge.swap) << ',' << value << ','
               << static_cast<double>(waves) / root_probes << ',' << safe << ',' << unsafe << ','
               << root_probes - safe - unsafe << ',' << cap << ',' << depth << ',' << mass
               << ',' << 1 - mass << '\n';
    }
    std::sort(values.begin(), values.end(), std::greater<>());
    if (values.size() > 1) {
        result.gap = values[0] - values[1];
        const double peak = values[0];
        for (auto& value : values) {
            value = std::exp(value - peak);
        }
        const auto total = std::accumulate(values.begin(), values.end(), 0.0);
        for (double value : values) {
            const auto p = value / total;
            result.entropy -= p * std::log(p);
        }
        result.entropy /= std::log(static_cast<double>(values.size()));
    }
    const auto samples = static_cast<double>(result.samples);
    result.waves /= samples;
    result.wave_variance = wave_squares / samples - result.waves * result.waves;
    result.impact = impacted / (samples * state.board.cells.size());
    result.lower = total_safe / samples;
    result.upper = 1 - total_unsafe / samples;
    result.witness /= static_cast<double>(root.edges.size());
    return result;
}

struct PolicyChoice {
    std::vector<std::size_t> edges;
    int action_evaluations = 0;
    int refill_probes = 0;
};

struct NaturalChoiceValue {
    int progress = 0;
    int finish = 0;
    int overshoot = 0;
    int lookahead = 0;
    int future_evaluations = 0;
};

NaturalChoiceValue evaluate_choice(const m3::State& state, m3::Swap swap,
                                   const m3::Rules& rules, std::uint64_t seed, bool look_ahead)
{
    NaturalChoiceValue total;
    std::mt19937_64 random(seed);
    for (int sample = 0; sample < policy_probes; ++sample) {
        const auto turn = natural_turn(state.board, swap, rules, random);
        if (turn.stop != Stop::stable) {
            continue;
        }
        const int remaining = std::max(0, state.goal - turn.cleared);
        total.progress += state.goal - remaining;
        total.finish += remaining == 0;
        total.overshoot += std::max(0, turn.cleared - state.goal);
        if (look_ahead) {
            int next_best = 0;
            if (remaining > 0 && state.moves > 1) {
                for (auto next : m3::legal_swaps(turn.board)) {
                    const auto clear = immediate_clears(turn.board, next);
                    if (clear <= rules.refill_cap) {
                        next_best = std::max(next_best, std::min(remaining, clear));
                    }
                    ++total.future_evaluations;
                }
            }
            total.lookahead += 2 * turn.cleared + std::max(0, turn.waves - 1) +
                               next_best + 6 * (remaining == 0);
        }
    }
    return total;
}

struct Episode {
    int clears = 0;
    int left = 0;
    int waves = 0;
    int extra_waves = 0;
    int action_evaluations = 0;
    int refill_probes = 0;
    bool chain = false;
};

int edge_clears(const m3::Edge& edge)
{
    int count = 0;
    for (const auto& batch : edge.refills) {
        count += static_cast<int>(batch.size());
    }
    return count;
}

using Exact = std::array<double, 3>;

class Policy {
public:
    Policy(const m3::Certificate& cert, const m3::Rules& rules, std::uint64_t seed, int index)
        : cert_(cert), rules_(rules), seed_(seed), index_(index), choices_(cert.nodes.size())
    {
    }

    Episode replay(std::mt19937_64& random)
    {
        std::size_t index = cert_.root;
        Episode totals;
        while (cert_.nodes[index].state.goal > 0) {
            const auto& node = cert_.nodes[index];
            const auto& choice = choices(index);
            const auto& edge = node.edges[choice.edges[m3::board_detail::uniform_below(random, choice.edges.size())]];
            const auto waves = static_cast<int>(edge.refills.size());
            totals.clears += edge_clears(edge);
            totals.waves += waves;
            totals.extra_waves += waves - 1;
            totals.chain = totals.chain || waves > 1;
            totals.action_evaluations += choice.action_evaluations;
            totals.refill_probes += choice.refill_probes;
            index = edge.child;
        }
        totals.left = cert_.nodes[index].state.moves;
        require(totals.clears >= cert_.nodes[cert_.root].state.goal, "certified episode missed objective");
        return totals;
    }

    Exact exact(int surplus)
    {
        return exact_from(cert_.root, cert_.nodes[cert_.root].state.goal + surplus, false);
    }

private:
    const PolicyChoice& choices(std::size_t index)
    {
        auto& choice = choices_[index];
        if (!choice) {
            choice = evaluate(index);
        }
        return *choice;
    }

    PolicyChoice evaluate(std::size_t index) const
    {
        const auto& node = cert_.nodes[index];
        PolicyChoice result;
        require(!node.edges.empty(), "nonterminal certificate node has no edges");
        if (index_ == 0) {
            result.edges.resize(node.edges.size());
            std::iota(result.edges.begin(), result.edges.end(), 0);
            return result;
        }
        const auto& state = node.state;
        std::optional<std::array<int, 3>> maximum;
        for (std::size_t edge_index = 0; edge_index < node.edges.size(); ++edge_index) {
            const auto swap = node.edges[edge_index].swap;
            std::array<int, 3> value{};
            ++result.action_evaluations;
            if (index_ == 1) {
                value[0] = immediate_clears(state.board, swap);
            } else {
                const auto probes_seed = mix(seed_ ^ 0xdd54aa90ULL ^ (index * 131071) ^
                                             (swap.a * 257 + swap.b));
                const auto v = evaluate_choice(state, swap, rules_, probes_seed, index_ == 3);
                result.refill_probes += policy_probes;
                result.action_evaluations += v.future_evaluations;
                value = index_ == 2 ? std::array{v.progress, v.finish, -v.overshoot}
                                    : std::array{v.lookahead, 0, 0};
            }
            if (!maximum || value > *maximum) {
                maximum = value;
                result.edges.clear();
            }
            if (value == *maximum) {
                result.edges.push_back(edge_index);
            }
        }
        return result;
    }

    Exact exact_from(std::size_t index, int remaining_challenge, bool has_chain)
    {
        const auto& node = cert_.nodes[index];
        if (node.state.goal <= 0) {
            return {static_cast<double>(remaining_challenge == 0),
                    static_cast<double>(node.state.moves > 0), static_cast<double>(has_chain)};
        }
        const auto key = (static_cast<std::uint64_t>(index) << 33) |
                         (static_cast<std::uint64_t>(remaining_challenge) << 1) | has_chain;
        if (const auto found = exact_.find(key); found != exact_.end()) {
            return found->second;
        }
        const auto& chosen = choices(index).edges;
        Exact sum{};
        for (auto edge_index : chosen) {
            const auto& edge = node.edges[edge_index];
            const auto probabilities = exact_from(edge.child,
                std::max(0, remaining_challenge - edge_clears(edge)), has_chain || edge.refills.size() > 1);
            for (std::size_t metric = 0; metric < sum.size(); ++metric) {
                sum[metric] += probabilities[metric] / static_cast<double>(chosen.size());
            }
        }
        exact_.emplace(key, sum);
        return sum;
    }

    const m3::Certificate& cert_;
    const m3::Rules& rules_;
    std::uint64_t seed_;
    int index_;
    std::vector<std::optional<PolicyChoice>> choices_;
    std::unordered_map<std::uint64_t, Exact> exact_;
};

void write_policy(const std::string& id, const m3::Certificate& cert, const m3::Rules& rules,
                  std::uint64_t seed, int agent, std::ostream& output)
{
    Policy policy(cert, rules, seed, agent);
    Episode sum;
    int challenge = 0;
    int early = 0;
    int chains = 0;
    const int threshold = cert.nodes[cert.root].state.goal + challenge_extra;
    std::mt19937_64 random(mix(seed ^ 0x9e3779b97f4a7c15ULL));
    for (int count = 0; count < rolls; ++count) {
        const auto run = policy.replay(random);
        sum.clears += run.clears;
        sum.left += run.left;
        sum.waves += run.waves;
        sum.extra_waves += run.extra_waves;
        sum.action_evaluations += run.action_evaluations;
        sum.refill_probes += run.refill_probes;
        challenge += run.clears >= threshold;
        early += run.left > 0;
        chains += run.chain;
    }
    const auto [exact_challenge, exact_early, exact_chain] = policy.exact(challenge_extra);
    const auto surplus_1 = policy.exact(1)[0];
    const auto surplus_6 = policy.exact(6)[0];
    const auto n = static_cast<double>(rolls);
    output << id << ',' << agents[agent] << ',' << rolls << ',' << rolls << ','
           << sum.clears / n << ',' << sum.left / n << ',' << sum.waves / n << ','
           << sum.extra_waves / n << ',' << sum.action_evaluations / n << ','
           << sum.refill_probes / n << ',' << challenge << ',' << early << ',' << chains << ','
           << exact_challenge << ',' << exact_early << ',' << exact_chain << ','
           << surplus_1 << ',' << surplus_6 << '\n';
}

std::vector<std::string> csv_fields(const std::string& line)
{
    std::vector<std::string> fields;
    std::istringstream stream(line);
    for (std::string field; std::getline(stream, field, ',');) {
        fields.push_back(std::move(field));
    }
    return fields;
}

strict::Problem read_problem(const fs::path& file, int goal, int moves, int cap)
{
    std::ifstream input(file);
    require(static_cast<bool>(input), "can't read board: " + file.string());
    strict::Board board;
    for (std::string line; std::getline(input, line);) {
        std::istringstream row(line);
        int columns = 0;
        for (int color; row >> color;) {
            require(color >= 0 && color < board.colors, "invalid board color");
            board.cells.push_back(static_cast<std::uint8_t>(color));
            ++columns;
        }
        if (columns > 0) {
            if (board.rows == 0) {
                board.columns = columns;
            }
            require(columns == board.columns, "inconsistent board width");
            ++board.rows;
        }
    }
    return {std::move(board), goal, moves, {cap}};
}

double color_entropy(const m3::Board& board)
{
    std::vector<int> counts(board.colors);
    for (auto color : board.cells) {
        ++counts[color];
    }
    double sum = 0;
    for (int count : counts) {
        if (count != 0) {
            const double p = count / static_cast<double>(board.cells.size());
            sum -= p * std::log(p);
        }
    }
    return sum / std::log(static_cast<double>(board.colors));
}

} // namespace

int main(int argc, char** argv) try {
    require(argc == 3 || argc == 4, "usage: simulate DATASET_DIR RESULTS_DIR [BOARD_LIMIT]");
    const fs::path dataset = argv[1];
    const fs::path results = argv[2];
    const int limit = argc == 4 ? std::stoi(argv[3]) : std::numeric_limits<int>::max();
    fs::create_directories(results);
    std::ifstream candidates(dataset / "candidates.csv");
    require(static_cast<bool>(candidates), "cannot read candidates.csv");
    std::string line;
    std::getline(candidates, line);
    std::unordered_map<std::string, std::size_t> column;
    for (const auto& field : csv_fields(line)) {
        const auto index = column.size();
        column.emplace(field, index);
    }
    std::ofstream levels(results / "levels.csv");
    std::ofstream policies(results / "policies.csv");
    std::ofstream actions(results / "root_actions.csv");
    require(levels && policies && actions, "cannot create result tables");
    levels << "id,goal,moves,rows,cols,colors,legal_swaps,legal_swap_density,color_entropy,"
              "d1_pressure,natural_mean_clears,natural_rollouts,natural_goal_rate,natural_cap_rate,"
              "natural_depth_rate,natural_dead_rate,d2_entropy,d2_best_gap,root_cascade_mean,"
              "root_cascade_variance,root_impact_ratio,root_safe_mass_lower,root_safe_mass_upper,"
              "root_witness_mass,root_witness_tv,root_samples,certificate_nodes\n" << std::setprecision(12);
    policies << "id,agent,runs,wins,mean_clears,mean_moves_left,mean_waves,mean_extra_waves,"
                "mean_action_evaluations,mean_refill_probes,challenge_count,early_count,chain_count,"
                "exact_challenge,exact_early,exact_chain,exact_surplus_1,exact_surplus_6\n"
             << std::setprecision(12);
    actions << "id,swap_a,swap_b,samples,immediate_clears,natural_mean_clears,natural_mean_waves,"
               "known_safe,known_unsafe,unresolved,cap_stops,depth_stops,witness_mass,witness_tv\n"
            << std::setprecision(12);

    std::unordered_set<std::string> seen;
    int completed = 0;
    while (completed < limit && std::getline(candidates, line)) {
        const auto fields = csv_fields(line);
        const auto field = [&](const std::string& key) -> const std::string& {
            return fields.at(column.at(key));
        };
        const auto id = field("id");
        if (field("verdict") != "WIN_CERTIFIED" || field("cert_verified") != "1" ||
            !seen.insert(id).second) {
            continue;
        }
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
        const auto problem = read_problem(dataset / "boards" / (id + ".txt"), goal, moves, cap);
        require(strict::verify_certificate(problem, certificate, depth), "independent certificate replay failed: " + id);
        encoded.seekg(0);
        const auto [rules, cert] = m3::read_certificate(encoded);
        const auto& root = cert.nodes.at(cert.root);
        const auto& state = root.state;
        require(state.goal > 0 && state.moves > 0 && !root.edges.empty(), "trivial level: " + id);
        require(rules.refill_cap == 12 && rules.cascade_budget == 9, "unexpected experimental rules: " + id);

        const auto natural = play_natural(state, rules, seed);
        const auto features = measure_root(id, cert, rules, seed, actions);
        const auto& board = state.board;
        const int adjacent = board.rows * (board.cols - 1) + board.cols * (board.rows - 1);
        const auto swaps = static_cast<double>(root.edges.size());
        const auto baseline_clear = natural.cleared / static_cast<double>(natural.swaps);
        require(natural.swaps > 0 && baseline_clear > 0, "natural baseline has no clear: " + id);
        const auto n = static_cast<double>(natural_rolls);
        levels << id << ',' << goal << ',' << moves << ',' << board.rows << ',' << board.cols << ','
               << board.colors << ',' << root.edges.size() << ',' << swaps / adjacent << ','
               << color_entropy(board) << ',' << goal / (moves * baseline_clear) << ',' << baseline_clear
               << ',' << natural_rolls << ',' << natural.goals / n << ',' << natural.caps / n << ','
               << natural.depths / n << ',' << natural.dead / n << ',' << features.entropy << ','
               << features.gap << ',' << features.waves << ',' << features.wave_variance << ','
               << features.impact << ',' << features.lower << ',' << features.upper << ','
               << features.witness << ',' << 1 - features.witness << ',' << features.samples << ','
               << cert.nodes.size() << '\n';
        for (int agent = 0; agent < static_cast<int>(agents.size()); ++agent) {
            write_policy(id, cert, rules, seed, agent, policies);
        }
        ++completed;
        levels.flush();
        policies.flush();
        actions.flush();
        if (completed % 10 == 0) {
            std::cerr << "simulated " << completed << " certified boards\n";
        }
    }
    require(completed > 0 && levels && policies && actions, "empty or incomplete simulation output");
    std::cerr << "simulated " << completed << " certified boards total\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "Simulation failed: " << error.what() << '\n';
    return 1;
}
