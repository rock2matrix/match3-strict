#include <match3_strict/solver.hpp>

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using namespace match3_strict;

constexpr int rows = 8;
constexpr int columns = 8;
constexpr int colors = 5;
constexpr int max_refill = 12;
constexpr SearchBudget budget{256'000, 720'000, 6'000, 9};

struct Stratum {
    int goal;
    int moves;
    std::uint64_t first_seed;
};

constexpr std::array strata{
    Stratum{6, 3, 1'000'000},
    Stratum{10, 4, 2'000'000},
    Stratum{8, 2, 3'000'000},
    Stratum{10, 3, 4'000'000},
};

int positive_arg(std::string_view value)
{
    int number = 0;
    const auto* end = value.data() + value.size();
    const auto [parsed, error] = std::from_chars(value.data(), end, number);
    if (error != std::errc{} || parsed != end || number <= 0 || number >= 1'000'000) {
        throw std::invalid_argument("quota and candidate limit must be in [1,999999]");
    }
    return number;
}

int uniform_below(std::mt19937_64& random, std::uint64_t bound)
{
    const auto threshold = (std::uint64_t{0} - bound) % bound;
    auto value = random();
    while (value < threshold) {
        value = random();
    }
    return static_cast<int>(value % bound);
}

// Match the historical random_stable layout (layout 0). This samples each
// valid local color uniformly, not all possible stable boards uniformly.
Board random_stable_board(std::uint64_t seed)
{
    Board board{rows, columns, colors, std::vector<std::uint8_t>(rows * columns)};
    std::mt19937_64 random(seed);
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            const int index = row * columns + column;
            std::array<int, colors> choices{};
            int count = 0;
            for (int color = 0; color < colors; ++color) {
                const bool row_match = column >= 2 && board.cells[index - 1] == color &&
                    board.cells[index - 2] == color;
                const bool column_match = row >= 2 && board.cells[index - columns] == color &&
                    board.cells[index - 2 * columns] == color;
                if (!row_match && !column_match) {
                    choices[count++] = color;
                }
            }
            board.cells[index] = static_cast<std::uint8_t>(choices[uniform_below(random, count)]);
        }
    }
    return board;
}

std::ofstream open_output(const fs::path& path)
{
    std::ofstream output;
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output.open(path, std::ios::binary);
    return output;
}

void write_board(const fs::path& path, const Board& board)
{
    auto output = open_output(path);
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            if (column > 0) {
                output << ' ';
            }
            output << static_cast<int>(board.cells[row * columns + column]);
        }
        output << '\n';
    }
    output.close();
}

double micros(Clock::duration duration)
{
    return std::chrono::duration<double, std::micro>(duration).count();
}

int generate(const fs::path& path, int quota, int candidate_limit)
{
    if (fs::exists(path) && !fs::is_empty(path)) {
        throw std::invalid_argument("output directory must be new or empty");
    }
    fs::create_directories(path / "boards");
    fs::create_directories(path / "certificates");
    auto candidates = open_output(path / "candidates.csv");
    candidates << "id,seed,stratum,goal,moves,verdict,cert_verified,candidate_checks,refill_nodes,"
                  "states,legal_swaps,safe_swaps,candidate_cutoffs,refill_cutoffs,state_cutoffs,"
                  "cascade_cutoffs,certificate_limit,solve_us,verify_us\n";
    candidates << std::fixed << std::setprecision(3);
    auto settings = open_output(path / "settings.csv");
    settings << "stratum,goal,moves,first_seed,certified_quota,candidate_limit,rows,columns,colors,"
                "max_refill,max_candidates,max_refill_nodes,max_states,cascade_limit,board_generator\n";
    for (const auto& stratum : strata) {
        settings << 'g' << stratum.goal << "-m" << stratum.moves << ',' << stratum.goal << ','
                 << stratum.moves << ',' << stratum.first_seed << ',' << quota << ',' << candidate_limit
                 << ',' << rows << ',' << columns << ',' << colors << ',' << max_refill << ','
                 << budget.max_candidates << ',' << budget.max_refill_nodes << ',' << budget.max_states
                 << ',' << budget.cascade_limit << ",historical_random_stable_layout_0\n";
    }
    settings.close();

    std::unordered_set<std::string> unique_boards;
    bool all_quotas_met = true;
    for (const auto& stratum : strata) {
        const auto label = "g" + std::to_string(stratum.goal) + "-m" + std::to_string(stratum.moves);
        int certified = 0;
        int attempted = 0;
        for (int offset = 0; offset < candidate_limit && certified < quota; ++offset) {
            const auto seed = stratum.first_seed + offset;
            const auto id = "s" + std::to_string(seed) + '-' + label;
            const Problem problem{random_stable_board(seed), stratum.goal, stratum.moves, {max_refill}};
            std::string board_key(problem.board.cells.begin(), problem.board.cells.end());
            if (!unique_boards.insert(std::move(board_key)).second) {
                throw std::runtime_error("duplicate generated board for " + id);
            }
            ++attempted;
            write_board(path / "boards" / (id + ".txt"), problem.board);
            const auto solve_start = Clock::now();
            const auto result = solve(problem, budget);
            const auto solve_duration = Clock::now() - solve_start;
            bool verified = false;
            double verify_us = 0.0;
            if (result.verdict == Verdict::win_certified) {
                const auto verify_start = Clock::now();
                verified = verify_certificate(problem, result.certificate, budget.cascade_limit);
                verify_us = micros(Clock::now() - verify_start);
            }
            const auto& work = result.work;
            candidates << id << ',' << seed << ',' << label << ',' << stratum.goal << ',' << stratum.moves
                       << ',' << verdict_name(result.verdict) << ',' << verified << ',' << work.candidate_checks
                       << ',' << work.refill_nodes << ',' << work.states << ',' << work.legal_swaps << ','
                       << work.safe_swaps << ',' << work.candidate_cutoffs << ',' << work.refill_cutoffs << ','
                       << work.state_cutoffs << ',' << work.cascade_cutoffs << ',' << work.certificate_limit
                       << ',' << micros(solve_duration) << ',' << verify_us << '\n';
            candidates.flush();
            if (result.verdict == Verdict::win_certified && !verified) {
                throw std::runtime_error("certificate verification failed for " + id);
            }
            if (verified) {
                auto output = open_output(path / "certificates" / (id + ".m3cert"));
                output << result.certificate;
                output.close();
                ++certified;
            }
        }
        std::cerr << label << ' ' << certified << '/' << quota << " certified from " << attempted << " candidates\n";
        all_quotas_met = all_quotas_met && certified == quota;
    }
    candidates.close();
    return all_quotas_met ? 0 : 2;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc < 2 || argc > 4) {
            std::cerr << "usage: generate OUTPUT_DIRECTORY [CERTIFIED_QUOTA] [CANDIDATE_LIMIT]\n";
            return 1;
        }
        const int quota = argc > 2 ? positive_arg(argv[2]) : 25;
        const int candidate_limit = argc > 3 ? positive_arg(argv[3]) : 1'000;
        return generate(argv[1], quota, candidate_limit);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
