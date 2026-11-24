#include <match3_strict/solver.hpp>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

namespace strict = match3_strict;

void require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

strict::Problem known_problem()
{
    return {{9, 7, 5, {
        3, 0, 3, 1, 4, 3, 1,
        3, 2, 0, 4, 3, 0, 4,
        4, 3, 0, 3, 1, 0, 2,
        4, 1, 4, 0, 4, 3, 2,
        3, 2, 4, 1, 1, 2, 4,
        0, 1, 2, 1, 1, 3, 0,
        2, 1, 3, 0, 4, 0, 3,
        4, 4, 2, 2, 0, 1, 3,
        4, 4, 0, 3, 1, 1, 2,
    }}, 6, 2};
}

void verify_win_and_certificate()
{
    const auto initial = known_problem();
    const auto result = strict::solve(initial);
    require(result.verdict == strict::Verdict::win_certified, "known board was not certified");
    require(strict::verdict_name(result.verdict) == "WIN_CERTIFIED", "wrong WIN spelling");
    require(result.work.legal_swaps == 9 && result.work.safe_swaps == 9, "certificate must cover nine root swaps");
    require(!result.certificate.empty(), "WIN should return a certificate");
    require(strict::verify_certificate(initial, result.certificate), "independent verifier rejected WIN");

    auto different = initial;
    ++different.goal;
    require(!strict::verify_certificate(different, result.certificate), "wrong target accepted");
    different = initial;
    ++different.moves;
    require(!strict::verify_certificate(different, result.certificate), "wrong move limit accepted");
    different = initial;
    different.board.cells.back() = 0;
    require(!strict::verify_certificate(different, result.certificate), "wrong board accepted");
    different = initial;
    --different.rules.max_refill;
    require(!strict::verify_certificate(different, result.certificate), "wrong refill rule accepted");
    require(!strict::verify_certificate(initial, result.certificate, 8), "wrong cascade limit accepted");
    const std::string_view text = result.certificate;
    require(!strict::verify_certificate(initial, text.substr(0, text.size() / 2)), "truncated certificate accepted");
    require(!strict::verify_certificate(initial, result.certificate + " extra"), "trailing tamper accepted");
}

void verify_fail_and_unknown()
{
    strict::Board dead{5, 5, 5, {}};
    for (int row = 0; row < dead.rows; ++row) {
        for (int column = 0; column < dead.columns; ++column) {
            dead.cells.push_back(static_cast<std::uint8_t>((row + column) % dead.colors));
        }
    }
    const auto fail = strict::solve({dead, 3, 2});
    require(fail.verdict == strict::Verdict::fail_certified, "dead board should provide definitive failure");
    require(strict::verdict_name(fail.verdict) == "FAIL_CERTIFIED", "wrong FAIL spelling");
    require(fail.work.legal_swaps == 0 && fail.certificate.empty(), "incorrect dead-board work or output");

    strict::SearchBudget zero;
    zero.max_candidates = 0;
    const auto unknown = strict::solve(known_problem(), zero);
    require(unknown.verdict == strict::Verdict::unknown, "zero candidate budget must leave the known board UNKNOWN");
    require(strict::verdict_name(unknown.verdict) == "UNKNOWN", "wrong UNKNOWN spelling");
    require(unknown.work.candidate_checks == 0 && unknown.certificate.empty(), "zero-budget accounting is inconsistent");
}

void expect_invalid(const strict::Problem& problem)
{
    try {
        (void)strict::solve(problem);
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("invalid problem accepted");
}

void verify_invalid_inputs()
{
    auto invalid = known_problem();
    invalid.board.cells.pop_back();
    expect_invalid(invalid);

    invalid = known_problem();
    invalid.board.cells.front() = 5;
    expect_invalid(invalid);

    invalid = known_problem();
    invalid.board.cells[0] = invalid.board.cells[1] = invalid.board.cells[2] = 0;
    expect_invalid(invalid);

    invalid = known_problem();
    invalid.goal = -1;
    expect_invalid(invalid);
}

} // namespace

int main() try {
    verify_win_and_certificate();
    verify_fail_and_unknown();
    verify_invalid_inputs();
    std::cout << "Strict solver regression checks passed.\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
