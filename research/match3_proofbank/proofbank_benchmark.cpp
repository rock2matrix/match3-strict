#include "acr_for_proofbank.inc"
#include "expansion.hpp"

namespace {

constexpr std::uint64_t base_discovery_seed = 202611245;
constexpr std::uint64_t profile_seed = 202611251;
constexpr std::uint64_t audit_seed = 202611252;

struct Results {
    AcrBoardOutput gameplay;
    std::string profile;
    std::string queries;
    std::string expansion;
    std::string audit;
};

Results evaluate_board(const fs::path& dataset, const fs::path& archive, const BoardSpec& board,
                       int runs, std::uint64_t seed, bool warm, proofbank::Variant variant,
                       std::uint32_t extra_limit, const std::vector<ControllerSpec>& controllers)
{
    std::ofstream store;
    if (!archive.empty()) {
        require(!board.id.empty() && board.id.find_first_not_of(
                    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_") == std::string::npos,
                "unsafe board id for archive output");
        store.open(archive / (board.id + ".m3bank"), std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(store), "cannot open per-board bank archive");
    }
    const auto prepared = proofbank::prepare_expanded_board(dataset, board, base_discovery_seed, profile_seed,
                                                           audit_seed, variant, extra_limit, store.is_open() ? &store : nullptr);
    if (store.is_open()) {
        store.close();
        require(static_cast<bool>(store), "cannot write per-board bank archive");
    }
    std::ostringstream profiles, queries, expansion, audit;
    for (auto* out : {&profiles, &queries, &expansion, &audit}) {
        *out << std::setprecision(15);
    }
    proofbank::write_profile_csv_rows(profiles, prepared);
    proofbank::write_queries_csv_rows(queries, prepared);
    proofbank::write_expansion_csv_row(expansion, prepared);
    proofbank::write_audit_csv_rows(audit, prepared);
    auto order = controllers;
    if (board.seed % 2 != 0) {
        std::reverse(order.begin(), order.end());
    }
    return {evaluate_prepared_board(prepared.board.get(), runs, seed, warm, order),
            profiles.str(), queries.str(), expansion.str(), audit.str()};
}

std::vector<ControllerSpec> selected_controllers(std::string_view choice)
{
    if (choice == "pair") {
        return {{"Fixed6", 6}, {"Adaptive", -1}};
    }
    require(choice == "adaptive", "controller selection must be pair or adaptive");
    return controller_specs(choice);
}

void write_results(const fs::path& output, const std::vector<Results>& results)
{
    std::ofstream summary(output / "summary.csv"), episodes(output / "episodes.csv"), turns(output / "turns.csv"),
        mass(output / "mass.csv"), references(output / "witness-exact.csv"), offline(output / "offline-bank.csv"),
        profiles(output / "profile.csv"), queries(output / "queries.csv"),
        expansion(output / "expansion.csv"), audit(output / "matched-audit.csv");
    require(summary && episodes && turns && mass && references && offline && profiles && queries && expansion && audit,
            "cannot open comparison output tables");

    summary << "id,seed,recipe,goal,moves,agent,controller,episodes,wins,early,challenge,chain,productive_extra,clears,"
               "total_turns,executed_witness,executed_proposal,witness_fallbacks,proposal_attempts,proposal_safe,"
               "proposal_unsafe,proposal_unknown,proposal_same_witness,candidate_checks,refill_nodes,states,"
               "cache_hits,timeout_fallbacks,cap_fallbacks,extra_waves,max_waves,planned_witness_turns\n";
    episodes << "id,agent,controller,run,turns,clears,left,chain,productive_extra,extra_waves,max_waves,proposals,fallbacks\n";
    turns << "id,agent,controller,run,turn,remaining_goal,remaining_moves,swap_a,swap_b,bucket,z_point,z_lower,z_upper,witness_p0,"
             "k,forecast_fallback,candidate_checks,refill_nodes,states,proposal_safe,proposal_unsafe,proposal_unknown,cache_hits,"
             "used_witness,fallback,timeout,capped,same_witness,cpu_ns,wall_ns,clear,waves,first_wave,first_wave_witness,planned_witness,"
             "state_cells,state_fingerprint,action_fingerprint\n";
    mass << "id,recipe,bucket,slots,safe,unsafe,unresolved,forecast_samples,z_point,z_lower,z_upper,source,borrowed,k,"
            "forecast_fallback,forecast_objective\n";
    references << "id,agent,exact_early,exact_challenge,exact_chain,exact_mean_productive_extra_tiles,"
                   "paired_exact_early,paired_exact_challenge,paired_exact_chain,paired_exact_mean_productive_extra_tiles\n";
    offline << "id,recipe,cpu_ns,wall_ns,proposals,solver_calls,certificates_verified,natural_edges_replayed,"
               "retained_certificate_nodes,candidate_checks,refill_nodes,states";
    for (const auto label : refill_study::outcome_names) {
        offline << ',' << label;
    }
    offline << '\n';
    proofbank::write_profile_csv_header(profiles);
    proofbank::write_queries_csv_header(queries);
    proofbank::write_expansion_csv_header(expansion);
    proofbank::write_audit_csv_header(audit);
    for (const auto& result : results) {
        summary << result.gameplay.summary;
        episodes << result.gameplay.episodes;
        turns << result.gameplay.turns;
        mass << result.gameplay.mass;
        references << result.gameplay.references;
        offline << result.gameplay.offline;
        profiles << result.profile;
        queries << result.queries;
        expansion << result.expansion;
        audit << result.audit;
    }
    for (auto* out : {&summary, &episodes, &turns, &mass, &references, &offline, &profiles, &queries, &expansion, &audit}) {
        out->close();
        require(static_cast<bool>(*out), "failed while writing comparison tables");
    }
}

} // namespace

int main(int argc, char** argv) try {
    require(argc >= 10 && argc <= 12,
            "usage: proofbank-benchmark DATASET OUTPUT EPISODES PER_RECIPE WORKERS GAMEPLAY_SEED "
            "CACHE (warm|cold) BANK (base|targeted) ARMS (pair|adaptive) [EXTRA_LIMIT] [archive|noarchive]");
    const fs::path dataset = argv[1], output = argv[2];
    const int episodes = std::stoi(argv[3]), quota = std::stoi(argv[4]), workers = std::stoi(argv[5]);
    const auto seed = std::stoull(argv[6], nullptr, 0);
    const std::string_view cache = argv[7], variant_name = argv[8], arm_name = argv[9];
    const auto controllers = selected_controllers(arm_name);
    const auto requested_limit = argc >= 11 ? std::stoul(argv[10]) : 240UL;
    const std::string_view archive_mode = argc >= 12 ? argv[11] : "noarchive";
    require(episodes > 0 && quota > 0 && workers > 0 && (cache == "warm" || cache == "cold") &&
                (variant_name == "base" || variant_name == "targeted") && requested_limit <= 240 &&
                (archive_mode == "noarchive" || (archive_mode == "archive" && variant_name == "targeted")),
            "invalid comparison options");
    require(seed != base_discovery_seed && seed != profile_seed && seed != audit_seed,
            "gameplay and offline phase seed must differ");
    require(!fs::exists(output), "output already exists");
    const auto variant = variant_name == "base" ? proofbank::Variant::Base : proofbank::Variant::Targeted;
    const auto extra_limit = variant_name == "base" ? 0U : static_cast<std::uint32_t>(requested_limit);
    const auto boards = read_boards(dataset, quota);
    auto staging = output;
    staging += ".partial";
    if (output.has_parent_path()) {
        fs::create_directories(output.parent_path());
    }
    require(fs::create_directory(staging), "comparison staging directory already exists");
    fs::path archive;
    if (archive_mode == "archive") {
        archive = staging / "bank-entries";
        fs::create_directory(archive);
    }

    const auto started = std::chrono::steady_clock::now();
    std::vector<Results> results(boards.size());
    std::atomic_size_t next{}, complete{};
    std::vector<std::future<void>> jobs;
    for (int worker = 0; worker < workers; ++worker) {
        jobs.push_back(std::async(std::launch::async, [&] {
            for (auto index = next++; index < boards.size(); index = next++) {
                try {
                    results[index] = evaluate_board(dataset, archive, boards[index], episodes, seed, cache == "warm",
                                                    variant, extra_limit, controllers);
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
    write_results(staging, results);
    std::ofstream settings(staging / "run-settings.csv");
    settings << "setting,value\nbank," << variant_name << "\ncache," << cache << "\narms," << arm_name
             << "\nboard_count," << boards.size() << "\nepisodes_per_agent_arm," << episodes
             << "\nworkers," << workers << "\ngameplay_seed," << seed << "\nbase_discovery_seed," << base_discovery_seed
             << "\nprofile_seed," << profile_seed << "\naudit_seed," << audit_seed << "\nextra_solver_limit," << extra_limit
             << "\npaired_arm_order,alternated_by_board_seed\n";
    settings.close();
    require(static_cast<bool>(settings), "failed while writing settings");
    fs::rename(staging, output);
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::cerr << "completed " << boards.size() << " boards in " << seconds << " s, bank=" << variant_name
              << ", cache=" << cache << '\n';
    return 0;
} catch (const std::exception& error) {
    std::cerr << "proof-bank benchmark failed: " << error.what() << '\n';
    return 1;
}
