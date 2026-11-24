#define main acr_batch_driver_main
#include "acr_benchmark.cpp"
#undef main

#include "acr_load_driver.hpp"

#include <iomanip>
#include <numeric>
#include <string_view>

namespace acr_load {

struct Fixture {
    const PreparedBoard* board;
    std::size_t board_index;
    ProofRef state;
    std::size_t edge;
    int adaptive_budget;
};

struct Arm {
    std::string_view name;
    int fixed_attempts;
};

constexpr std::array arms{
    Arm{"Witness", 0}, Arm{"Fixed1", 1}, Arm{"Fixed6", 6}, Arm{"ACR", -1},
};

const std::array scenarios{
    Scenario{"solo", 1, 3000, 5000, 128},
    Scenario{"four_worker", 4, 6000, 20000, 128},
    Scenario{"eight_worker", 8, 12000, 40000, 128},
    Scenario{"four_worker_burst", 4, 20000, 160000, 128},
    Scenario{"eight_worker_burst", 8, 24000, 320000, 128},
};

void collect_fixtures(const PreparedBoard& board, std::size_t board_index, std::vector<Fixture>& fixtures)
{
    const auto root = board.oracle->root();
    const auto& edges = root.node().edges;
    const auto sampled = std::min<std::size_t>(6, edges.size());
    const auto add = [&](ProofRef state, std::size_t edge) {
        fixtures.push_back({&board, board_index, state, edge, adaptive_k(board, state, state.node().edges.at(edge))});
    };
    for (std::size_t i = 0; i < sampled; ++i) {
        const auto edge_index = i * edges.size() / sampled;
        add(root, edge_index);
        const auto child = board.oracle->witness(root, edges[edge_index]).continuation;
        if (!child.certificate || child.node().state.goal <= 0 || child.node().edges.empty()) {
            continue;
        }
        const auto child_index = (board.board.seed + i) % child.node().edges.size();
        add(child, child_index);
    }
}

Decision execute(const Fixture& fixture, int attempts, std::uint64_t seed, LookupCache* cache)
{
    const auto& edge = fixture.state.node().edges.at(fixture.edge);
    const auto& state = fixture.state.node().state;
    const auto planned = attempts < 0 ? adaptive_k(*fixture.board, fixture.state, edge) : attempts;
    const auto choice = choose_refill(*fixture.board->oracle, fixture.state, edge, planned, seed, cache);
    const bool verified = !choice.refills.empty() && choice.step.moves == state.moves - 1 &&
        choice.step.goal == std::max(0, state.goal - choice.step.clear) &&
        (choice.step.goal == 0 || (choice.step.continuation.certificate &&
            choice.step.continuation.node().state.goal == choice.step.goal &&
            choice.step.continuation.node().state.moves == choice.step.moves)) &&
        (!choice.used_witness || choice.refills == edge.refills);
    return {static_cast<unsigned>(planned), static_cast<unsigned>(choice.candidate_checks),
            static_cast<unsigned>(choice.candidate_checks), static_cast<unsigned>(choice.refill_nodes),
            static_cast<unsigned>(choice.states), static_cast<unsigned>(!choice.used_witness),
            static_cast<unsigned>(choice.safe), static_cast<unsigned>(choice.cache_hits), choice.used_witness,
            choice.fallback, choice.timeout, choice.capped, verified};
}

void write_metrics(std::ostream& summary, std::ostream& requests, const Run& run,
                   std::string_view arm, std::string_view cache_mode, unsigned repeat, std::size_t fixture_count)
{
    std::vector<std::int64_t> service, waiting, cpu, elapsed, dispatch_delay;
    std::int64_t total_cpu = 0;
    unsigned admitted = 0, rejected = 0, witness = 0, fallback = 0, deadline_capped = 0, work_capped = 0,
             cpu_over_soft = 0, unverified = 0,
             safe = 0, replays = 0, cache_hits = 0, proposals = 0, planned = 0, max_attempts = 0,
             max_candidate_checks = 0, max_refill_nodes = 0, max_state_expansions = 0;
    for (unsigned index = 0; index < run.records.size(); ++index) {
        const auto& r = run.records[index];
        const auto& d = r.decision;
        requests << arm << ',' << cache_mode << ',' << run.scenario.name << ',' << repeat << ',' << index << ','
                 << r.worker << ',' << index % fixture_count << ',' << r.admitted << ',' << r.due_ns << ','
                 << r.enqueued_ns << ',';
        if (!r.admitted) {
            ++rejected;
            requests << ",,,,,,,,,,,,,,,\n";
            continue;
        }
        if (r.start_ns < r.enqueued_ns || r.finish_ns < r.start_ns || r.cpu_ns < 0 || d.proposals > d.planned ||
            d.planned > 6 || d.candidate_checks > 512 || d.refill_nodes > 8192 || d.state_expansions > 128) {
            throw std::runtime_error("an admitted request violated its time or work envelope");
        }
        ++admitted;
        service.push_back(r.finish_ns - r.start_ns);
        waiting.push_back(r.start_ns - r.enqueued_ns);
        elapsed.push_back(r.finish_ns - r.due_ns);
        dispatch_delay.push_back(r.enqueued_ns - r.due_ns);
        cpu.push_back(r.cpu_ns);
        total_cpu += r.cpu_ns;
        cpu_over_soft += r.cpu_ns > 2'000'000;
        witness += d.used_witness;
        fallback += d.fallback;
        deadline_capped += d.deadline_capped;
        work_capped += d.work_capped;
        unverified += !d.verified;
        planned += d.planned;
        proposals += d.proposals;
        safe += d.verified_natural;
        replays += d.natural_replays;
        cache_hits += d.cache_hits;
        max_attempts = std::max(max_attempts, d.proposals);
        max_candidate_checks = std::max(max_candidate_checks, d.candidate_checks);
        max_refill_nodes = std::max(max_refill_nodes, d.refill_nodes);
        max_state_expansions = std::max(max_state_expansions, d.state_expansions);
        requests << r.start_ns << ',' << r.finish_ns << ',' << r.cpu_ns << ',' << d.planned << ',' << d.proposals
                 << ',' << d.candidate_checks << ',' << d.refill_nodes << ',' << d.state_expansions << ','
                 << d.verified_natural << ',' << d.natural_replays << ',' << d.cache_hits << ','
                 << d.used_witness << ',' << d.fallback << ',' << d.deadline_capped << ',' << d.work_capped << ','
                 << d.verified << '\n';
    }
    const auto duration_s = run.duration_ns / 1'000'000'000.0;
    const auto samples = [&](const std::vector<std::int64_t>& v) {
        for (double p : {0.5, 0.95, 0.99}) {
            summary << ',' << percentile(v, p);
        }
    };
    summary << arm << ',' << cache_mode << ',' << run.scenario.name << ',' << repeat << ',' << run.scenario.workers
            << ',' << run.scenario.rate << ',' << run.scenario.capacity << ',' << run.records.size() << ',' << admitted
            << ',' << rejected << ',' << duration_s << ',' << admitted / duration_s << ',' << total_cpu / 1'000'000'000.0
            << ',' << total_cpu / 1'000'000'000.0 / duration_s << ','
            << *std::max_element(run.high_water.begin(), run.high_water.end()) << ',' << witness << ',' << fallback
            << ',' << deadline_capped << ',' << work_capped
            << ',' << cpu_over_soft << ',' << unverified << ',' << planned << ',' << proposals << ',' << safe << ',' << replays << ','
            << cache_hits << ',' << max_attempts << ',' << max_candidate_checks << ',' << max_refill_nodes << ','
            << max_state_expansions;
    samples(cpu);
    samples(service);
    samples(waiting);
    samples(elapsed);
    samples(dispatch_delay);
    summary << '\n';
    if (admitted + rejected != run.records.size() || witness + safe != admitted || unverified != 0) {
        throw std::runtime_error("request coverage or Strict verification failed");
    }
}

} // namespace acr_load

int main(int argc, char** argv) try {
    using namespace acr_load;
    require(argc >= 3 && argc <= 6, "usage: acr-load DATASET OUTPUT [BOARDS_PER_RECIPE [SEED [REPEATS]]]");
    const auto dataset = fs::path(argv[1]);
    const auto output = fs::path(argv[2]);
    const auto per_recipe = argc > 3 ? std::stoi(argv[3]) : 4;
    const auto seed = argc > 4 ? std::stoull(argv[4]) : 202611246ULL;
    const auto repeats = argc > 5 ? std::stoul(argv[5]) : 2;
    require(per_recipe > 0 && repeats > 0 && !fs::exists(output), "positive sizes and a new output directory required");
    const auto specs = read_boards(dataset, per_recipe);
    std::vector<std::unique_ptr<PreparedBoard>> prepared;
    std::vector<Fixture> fixtures;
    for (const auto& board : specs) {
        prepared.push_back(prepare_board(dataset, board, seed));
        collect_fixtures(*prepared.back(), prepared.size() - 1, fixtures);
    }
    require(!fixtures.empty(), "no certified fixture actions");
    fs::create_directories(output);
    std::ofstream actions(output / "fixtures.csv"), summary(output / "load-summary.csv"), requests(output / "load-requests.csv");
    actions << "fixture,id,recipe,goal_remaining,turns_remaining,swap_a,swap_b,acr_planned\n";
    for (std::size_t i = 0; i < fixtures.size(); ++i) {
        const auto& f = fixtures[i];
        const auto& node = f.state.node();
        const auto swap = node.edges.at(f.edge).swap;
        actions << i << ',' << f.board->board.id << ',' << f.board->board.recipe << ',' << node.state.goal
                << ',' << node.state.moves << ',' << swap.a << ',' << swap.b << ',' << f.adaptive_budget << '\n';
    }
    summary << "controller,cache,scenario,repeat,workers,requested_per_second,queue_capacity_per_worker,offered,admitted,"
               "rejected,duration_s,throughput_per_s,decision_cpu_s,decision_cpu_cores,max_queue_depth,witness_uses,witness_fallbacks,"
               "deadline_capped,work_capped,cpu_over_2ms,unverified,attempts_planned,attempts_used,verified_natural,natural_replays,cache_hits,"
               "max_attempts_used,max_candidate_checks,max_refill_nodes,max_state_expansions,";
    for (const auto* field : {"cpu", "service_wall", "queue", "due_to_finish", "dispatch_delay"}) {
        summary << field << "_p50_us," << field << "_p95_us," << field << "_p99_us";
        summary << (std::string_view(field) == "dispatch_delay" ? '\n' : ',');
    }
    summary << std::setprecision(9);
    requests << "controller,cache,scenario,repeat,index,worker,fixture,admitted,due_ns,enqueued_ns,start_ns,finish_ns,cpu_ns,"
                "attempts_planned,attempts_used,candidate_checks,refill_nodes,state_expansions,verified_natural,natural_replays,cache_hits,"
                "used_witness,witness_fallback,deadline_capped,work_capped,verified\n";

    for (unsigned repeat = 0; repeat < repeats; ++repeat) {
        for (const auto& scenario : scenarios) {
            for (bool warm : {false, true}) {
                for (unsigned arm_index = 0; arm_index < arms.size(); ++arm_index) {
                    const auto& arm = arms[(arm_index + repeat) % arms.size()];
                    const auto demand_seed = mix(seed ^ mix(repeat + 1001));
                    const auto result = run(scenario, [&](unsigned worker) {
                        std::vector<LookupCache> cache(prepared.size());
                        if (warm) {
                            const auto warm_seed = mix(seed ^ mix(worker + 3001));
                            for (std::size_t i = 0; i < fixtures.size() * 3; ++i) {
                                const auto& fixture = fixtures[i % fixtures.size()];
                                execute(fixture, arm.fixed_attempts, mix(warm_seed ^ mix(i + 4001)),
                                        &cache.at(fixture.board_index));
                            }
                        }
                        return [&, memo = std::move(cache), demand_seed](unsigned index) mutable {
                            const auto& fixture = fixtures[index % fixtures.size()];
                            return execute(fixture, arm.fixed_attempts, mix(demand_seed ^ mix(index + 7001)),
                                           warm ? &memo.at(fixture.board_index) : nullptr);
                        };
                    });
                    const std::string_view mode = warm ? "Warm" : "ColdNoMemo";
                    write_metrics(summary, requests, result, arm.name, mode, repeat, fixtures.size());
                    std::cerr << scenario.name << ',' << mode << ',' << arm.name << ',' << repeat << " complete\n";
                }
            }
        }
    }
    require(actions && summary && requests, "cannot write load experiment tables");
    std::cout << "Verified load scenarios completed on " << fixtures.size() << " certified state/action fixtures\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "ACR load error: " << error.what() << '\n';
    return 1;
}
