#include <match3_strict/solver.hpp>

#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {

namespace strict = match3_strict;

constexpr std::streamoff max_certificate_bytes = 32 * 1024 * 1024;
constexpr std::size_t max_board_file_bytes = 8 * 1024;

void print_usage(std::ostream& output)
{
    output << "Usage\n"
              "  match3-solve BOARD_FILE TARGET MOVES [--certificate PATH]\n"
              "  match3-solve verify BOARD_FILE TARGET MOVES CERTIFICATE\n"
              "  match3-solve --help\n\n"
              "Write one row of colors per line, separated by whitespace.\n"
              "The board must be a stable 5-16 by 5-16 rectangle with colors 0-4.\n"
              "TARGET is the number of tiles to clear and MOVES is the swap limit.\n"
              "A certificate is written for WIN_CERTIFIED when PATH is supplied.\n";
}

int parse_nonnegative(std::string_view token, const std::string& label)
{
    int value = 0;
    const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
    if (error != std::errc{} || end != token.data() + token.size() || value < 0) {
        throw std::invalid_argument(label + " must be a nonnegative integer");
    }
    return value;
}

strict::Board read_board(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not open board file " + path.string());
    }
    std::array<char, max_board_file_bytes + 1> bytes;
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (input.bad() || (!input && !input.eof())) {
        throw std::runtime_error("could not read board file " + path.string());
    }
    const auto size = static_cast<std::size_t>(input.gcount());
    if (size > max_board_file_bytes) {
        throw std::invalid_argument("board file exceeds 8 KiB");
    }
    std::istringstream lines(std::string(bytes.data(), size));
    strict::Board board{};
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(lines, line)) {
        ++line_number;
        if (line_number == 1 && line.starts_with("\xEF\xBB\xBF")) {
            line.erase(0, 3);
        }
        std::istringstream row(line);
        std::string token;
        int columns = 0;
        while (row >> token) {
            const std::string label = "color on line " + std::to_string(line_number);
            const int value = parse_nonnegative(token, label);
            if (value >= board.colors) {
                throw std::invalid_argument(label + " must be in 0-4");
            }
            if (++columns > 16) {
                throw std::invalid_argument("board rows cannot exceed 16 columns");
            }
            board.cells.push_back(static_cast<std::uint8_t>(value));
        }
        if (columns == 0) {
            continue;
        }
        if (board.rows == 0) {
            board.columns = columns;
        } else if (columns != board.columns) {
            throw std::invalid_argument("inconsistent row width on line " + std::to_string(line_number));
        }
        if (++board.rows > 16) {
            throw std::invalid_argument("board cannot exceed 16 rows");
        }
    }
    if (board.rows == 0) {
        throw std::invalid_argument("board file is empty");
    }
    return board;
}

strict::Problem read_problem(const std::filesystem::path& path, std::string_view target, std::string_view moves)
{
    return {read_board(path), parse_nonnegative(target, "TARGET"), parse_nonnegative(moves, "MOVES")};
}

std::string read_certificate(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("could not open certificate " + path.string());
    }
    const std::streamoff size = input.tellg();
    if (size < 0) {
        throw std::runtime_error("could not size certificate " + path.string());
    }
    if (size > max_certificate_bytes) {
        return {};
    }
    std::string text(static_cast<std::size_t>(size), '\0');
    input.seekg(0);
    if (!input.read(text.data(), size) || input.peek() != std::char_traits<char>::eof() || input.bad()) {
        throw std::runtime_error("could not read certificate " + path.string());
    }
    return text;
}

void write_certificate(const std::filesystem::path& board_path, const std::filesystem::path& path,
                       std::string_view text)
{
    std::error_code error;
    if (std::filesystem::equivalent(board_path, path, error)) {
        throw std::invalid_argument("certificate output must differ from the board file");
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("could not open certificate output " + path.string());
    }
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.close();
    if (!output) {
        throw std::runtime_error("could not write certificate " + path.string());
    }
}

void print_result(const strict::Result& result)
{
    const auto& work = result.work;
    std::cout << strict::verdict_name(result.verdict) << '\n'
              << "candidate_checks=" << work.candidate_checks << '\n'
              << "refill_nodes=" << work.refill_nodes << '\n'
              << "states=" << work.states << '\n'
              << "legal_swaps=" << work.legal_swaps << '\n'
              << "safe_swaps=" << work.safe_swaps << '\n'
              << "candidate_cutoffs=" << work.candidate_cutoffs << '\n'
              << "refill_cutoffs=" << work.refill_cutoffs << '\n'
              << "state_cutoffs=" << work.state_cutoffs << '\n'
              << "cascade_cutoffs=" << work.cascade_cutoffs << '\n'
              << "certificate_limit=" << std::boolalpha << work.certificate_limit << '\n';
}

} // namespace

int main(int argc, char** argv) try {
    if (argc == 2 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h")) {
        print_usage(std::cout);
        return 0;
    }
    if (argc >= 2 && std::string_view(argv[1]) == "verify") {
        if (argc != 6) {
            print_usage(std::cerr);
            return 1;
        }
        const auto problem = read_problem(argv[2], argv[3], argv[4]);
        const auto certificate = read_certificate(argv[5]);
        const bool accepted = strict::verify_certificate(problem, certificate);
        std::cout << (accepted ? "PASS\n" : "REJECTED\n");
        return accepted ? 0 : 2;
    }
    if (argc != 4 && !(argc == 6 && std::string_view(argv[4]) == "--certificate")) {
        print_usage(std::cerr);
        return 1;
    }

    const auto problem = read_problem(argv[1], argv[2], argv[3]);
    const auto result = strict::solve(problem);
    const bool writes_certificate = argc == 6 && result.verdict == strict::Verdict::win_certified;
    if (writes_certificate) {
        write_certificate(argv[1], argv[5], result.certificate);
    }
    print_result(result);
    if (argc == 6) {
        std::cout << "certificate_written=" << std::boolalpha << writes_certificate << '\n';
    }
    return 0;
} catch (const std::exception& error) {
    std::cerr << "Error " << error.what() << '\n';
    return 1;
}
