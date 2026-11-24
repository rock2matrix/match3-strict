#include "refill_oracle.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <numeric>

namespace {

using namespace refill_study;

void check(const char* filename)
{
    std::ifstream file(filename, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot open smoke-test certificate");
    }
    const std::string encoded{std::istreambuf_iterator<char>(file), {}};
    std::istringstream source(encoded);
    const auto [rules, cert] = m3::read_certificate(source);
    const auto& root = cert.nodes.at(cert.root).state;
    const auto& board = root.board;
    const strict::Problem problem{{board.rows, board.cols, board.colors, board.cells}, root.goal, root.moves,
                                  {rules.refill_cap}};
    m3::verification::require(strict::verify_certificate(problem, encoded, rules.cascade_budget),
                              "baseline proof failed independent replay");

    for (const auto attempts : {1, 6}) {
        Oracle oracle(cert, rules, {16'000, 45'000, 600, rules.cascade_budget});
        std::mt19937_64 random(91861);
        int witnessed_fallbacks = 0;
        for (int game = 0; game < 64; ++game) {
            auto cursor = oracle.root();
            int total = 0;
            int previous_moves = root.moves;
            while (cursor.certificate != nullptr) {
                const auto& node = cursor.node();
                const auto& edge = node.edges.at(m3::board_detail::uniform_below(random, node.edges.size()));
                auto step = oracle.witness(cursor, edge);
                bool accepted = false;
                for (int draw = 0; draw < attempts; ++draw) {
                    const auto proposal = oracle.probe(cursor, edge, random());
                    if (proposal.accepted()) {
                        if (proposal.same_witness) {
                            m3::verification::require(proposal.step.goal == step.goal &&
                                                          proposal.step.moves == step.moves &&
                                                          proposal.step.wave_sizes == step.wave_sizes,
                                                      "identical proposal and witness diverged");
                        }
                        step = proposal.step;
                        accepted = true;
                        break;
                    }
                }
                witnessed_fallbacks += !accepted;
                m3::verification::require(step.clear >= 3 && step.moves == previous_moves - 1 &&
                                              step.goal == std::max(0, node.state.goal - step.clear) &&
                                              step.clear == std::accumulate(step.wave_sizes.begin(), step.wave_sizes.end(), 0) &&
                                              step.waves == static_cast<int>(step.wave_sizes.size()) &&
                                              step.extra == step.clear - step.wave_sizes.front(),
                                          "step metrics and continuation disagree");
                m3::verification::require((step.continuation.certificate != nullptr) == (step.goal > 0),
                                          "continuation does not match playable state");
                cursor = step.continuation;
                previous_moves = step.moves;
                total += step.clear;
            }
            m3::verification::require(total >= root.goal, "certified game did not attain goal");
        }
        const auto& stats = oracle.stats();
        const auto outcomes_total = std::accumulate(stats.outcomes.begin(), stats.outcomes.end(), std::uint64_t{0});
        const auto accepted_total = std::accumulate(stats.outcomes.begin(), stats.outcomes.begin() + 3, std::uint64_t{0});
        m3::verification::require(outcomes_total == stats.proposals && accepted_total == stats.natural_edges_replayed &&
                                      stats.certificates_verified <= stats.solver_calls,
                                  "oracle telemetry does not reconcile");
        std::cout << filename << " attempts=" << attempts << " proposals=" << stats.proposals
                  << " accepted=" << accepted_total << " fallback=" << witnessed_fallbacks
                  << " solves=" << stats.solver_calls << " new_verified=" << stats.certificates_verified
                  << " nodes_retained=" << stats.retained_certificate_nodes << '\n';
    }
}

} // namespace

int main(int argc, char* argv[]) try {
    if (argc < 2) {
        throw std::runtime_error("usage: oracle-smoke CERTIFICATE [CERTIFICATE ...]");
    }
    for (int index = 1; index < argc; ++index) {
        check(argv[index]);
    }
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
