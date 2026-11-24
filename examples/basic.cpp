#include <match3_strict/solver.hpp>

#include <iostream>

int main()
{
    const match3_strict::Problem problem{
        .board = {
            .rows = 9,
            .columns = 7,
            .colors = 5,
            .cells = {
                3, 0, 3, 1, 4, 3, 1,
                3, 2, 0, 4, 3, 0, 4,
                4, 3, 0, 3, 1, 0, 2,
                4, 1, 4, 0, 4, 3, 2,
                3, 2, 4, 1, 1, 2, 4,
                0, 1, 2, 1, 1, 3, 0,
                2, 1, 3, 0, 4, 0, 3,
                4, 4, 2, 2, 0, 1, 3,
                4, 4, 0, 3, 1, 1, 2,
            },
        },
        .goal = 6,
        .moves = 2,
    };

    const auto result = match3_strict::solve(problem);
    std::cout << match3_strict::verdict_name(result.verdict) << '\n';
    if (result.verdict != match3_strict::Verdict::win_certified) {
        return 1;
    }
    if (!match3_strict::verify_certificate(problem, result.certificate)) {
        std::cerr << "Certificate verification failed.\n";
        return 1;
    }
    std::cout << "Certificate verified against the input problem.\n";
}
