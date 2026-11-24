#include "match3_strict/solver.hpp"

#include "detail/resume_solver.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <locale>
#include <ostream>
#include <stdexcept>
#include <streambuf>
#include <system_error>
#include <utility>

namespace match3_strict {
namespace {

constexpr int min_dimension = 5;
constexpr int max_dimension = 16;
constexpr int min_colors = 2;
constexpr int max_colors = 8;
constexpr int max_goal = 1'000'000;
constexpr int max_moves = 20;
constexpr int max_refill = 16;
constexpr int max_cascade = 32;
constexpr std::uint64_t max_candidates = 2'000'000;
constexpr std::uint64_t max_refill_nodes = 20'000'000;
constexpr std::uint64_t max_states = 50'000;
constexpr std::size_t max_certificate_bytes = 32 * 1024 * 1024;
constexpr std::size_t max_certificate_nodes = 200'000;

void require_input(bool valid, const char* message)
{
    if (!valid) {
        throw std::invalid_argument(message);
    }
}

m3::State make_state(const Problem& problem)
{
    const auto& board = problem.board;
    require_input(board.rows >= min_dimension && board.rows <= max_dimension &&
                  board.columns >= min_dimension && board.columns <= max_dimension,
                  "board rows and columns must each be between 5 and 16");
    require_input(board.colors >= min_colors && board.colors <= max_colors,
                  "board color count must be between 2 and 8");
    require_input(board.cells.size() == static_cast<std::size_t>(board.rows * board.columns),
                  "board cell count must equal rows times columns");
    require_input(std::ranges::all_of(board.cells, [&](std::uint8_t cell) {
        return cell < board.colors;
    }), "board cell color is outside the configured palette");
    require_input(problem.goal >= 0 && problem.goal <= max_goal,
                  "goal must be between 0 and 1000000");
    require_input(problem.moves >= 0 && problem.moves <= max_moves,
                  "moves must be between 0 and 20");

    m3::Board native(board.rows, board.columns, board.colors);
    native.cells = board.cells;
    require_input(m3::verification::matches(native).empty(), "starting board must have no match");
    return {std::move(native), problem.goal, problem.moves};
}

m3::Rules make_rules(const Rules& rules, int cascade_limit)
{
    require_input(rules.max_refill >= 0 && rules.max_refill <= max_refill,
                  "maximum refill size must be between 0 and 16");
    require_input(cascade_limit >= 0 && cascade_limit <= max_cascade,
                  "cascade limit must be between 0 and 32");
    return {rules.max_refill, cascade_limit};
}

m3::Options make_options(const SearchBudget& budget)
{
    require_input(budget.max_candidates <= max_candidates,
                  "candidate budget must be at most 2000000");
    require_input(budget.max_refill_nodes <= max_refill_nodes,
                  "refill node budget must be at most 20000000");
    require_input(budget.max_states <= max_states, "state budget must be at most 50000");
    m3::Options options;
    options.max_candidates = budget.max_candidates;
    options.max_refill_nodes = budget.max_refill_nodes;
    options.max_states = budget.max_states;
    return options;
}

// Keeps oversized winning proofs from turning into unbounded text allocations.
class CertificateBuffer : public std::streambuf {
public:
    bool limit_reached() const { return limit_reached_; }
    std::string take() { return std::move(bytes_); }

protected:
    std::streamsize xsputn(const char* data, std::streamsize count) override
    {
        if (count <= 0) {
            return 0;
        }
        const auto size = static_cast<std::size_t>(count);
        if (size > max_certificate_bytes - bytes_.size()) {
            limit_reached_ = true;
            return 0;
        }
        bytes_.append(data, size);
        return count;
    }

    int_type overflow(int_type value) override
    {
        if (traits_type::eq_int_type(value, traits_type::eof())) {
            return traits_type::not_eof(value);
        }
        const char byte = traits_type::to_char_type(value);
        return xsputn(&byte, 1) == 1 ? value : traits_type::eof();
    }

private:
    std::string bytes_;
    bool limit_reached_{};
};

class CertificateReader {
public:
    explicit CertificateReader(std::string_view encoded) : remaining_(encoded) {}

    std::string_view token()
    {
        constexpr std::string_view whitespace = " \t\n\r\f\v";
        const auto start = remaining_.find_first_not_of(whitespace);
        if (start == std::string_view::npos) {
            remaining_ = {};
            return {};
        }
        remaining_.remove_prefix(start);
        const auto text = remaining_.substr(0, remaining_.find_first_of(whitespace));
        remaining_.remove_prefix(text.size());
        return text;
    }

    template<class Integer>
    Integer number()
    {
        const auto text = token();
        require_input(!text.empty(), "missing certificate integer");
        Integer value = 0;
        const auto end = text.data() + text.size();
        const auto [parsed, error] = std::from_chars(text.data(), end, value);
        require_input(error == std::errc{} && parsed == end, "invalid certificate integer");
        return value;
    }

private:
    std::string_view remaining_;
};

m3::Certificate parse_certificate(std::string_view encoded, const m3::State& expected,
                                  const m3::Rules& rules)
{
    CertificateReader reader(encoded);
    require_input(reader.token() == "M3CERT" && reader.number<int>() == 1,
                  "unsupported certificate version");
    const int refill = reader.number<int>();
    const int cascade = reader.number<int>();
    require_input(refill == rules.refill_cap && cascade == rules.cascade_budget,
                  "certificate rules differ from expected rules");

    const auto root = reader.number<std::size_t>();
    const auto node_count = reader.number<std::size_t>();
    require_input(root < node_count && node_count <= max_certificate_nodes,
                  "invalid certificate node count or root index");
    const auto& shape = expected.board;
    const auto max_edges = static_cast<std::size_t>(
        shape.rows * (shape.cols - 1) + shape.cols * (shape.rows - 1));
    const auto read_color = [&] {
        const int color = reader.number<int>();
        require_input(color >= 0 && color < shape.colors, "invalid certificate color");
        return static_cast<std::uint8_t>(color);
    };
    m3::Certificate cert{{}, root};
    for (std::size_t index = 0; index < node_count; ++index) {
        const int rows = reader.number<int>();
        const int cols = reader.number<int>();
        const int colors = reader.number<int>();
        const int goal = reader.number<int>();
        const int moves = reader.number<int>();
        const auto edge_count = reader.number<std::size_t>();
        require_input(rows == shape.rows && cols == shape.cols && colors == shape.colors,
                      "certificate board shape or palette differs from expected board");
        require_input(goal >= 0 && goal <= expected.goal && moves >= 0 && moves <= expected.moves &&
                      edge_count <= max_edges,
                      "certificate state or edge count exceeds the expected problem");
        m3::Board board(rows, cols, colors);
        for (auto& cell : board.cells) {
            cell = read_color();
        }
        m3::State state{std::move(board), goal, moves};
        auto& node = cert.nodes.emplace_back(m3::CertificateNode{std::move(state), {}});
        for (std::size_t edge_index = 0; edge_index < edge_count; ++edge_index) {
            const m3::Swap action{reader.number<int>(), reader.number<int>()};
            const auto child = reader.number<std::size_t>();
            const auto batches = reader.number<std::size_t>();
            require_input(child < node_count && batches > 0 &&
                          batches <= static_cast<std::size_t>(rules.cascade_budget),
                          "invalid certificate child index or cascade length");
            auto& edge = node.edges.emplace_back(m3::Edge{action, {}, child});
            for (std::size_t wave = 0; wave < batches; ++wave) {
                const auto count = reader.number<std::size_t>();
                require_input(count >= 3 && count <= static_cast<std::size_t>(rules.refill_cap),
                              "invalid certificate refill size");
                auto& batch = edge.refills.emplace_back();
                for (std::size_t color_index = 0; color_index < count; ++color_index) {
                    batch.push_back(read_color());
                }
            }
        }
    }
    require_input(reader.token().empty(), "unexpected certificate trailing data");
    return cert;
}

} // namespace

std::string_view verdict_name(Verdict verdict) noexcept
{
    switch (verdict) {
    case Verdict::win_certified:
        return "WIN_CERTIFIED";
    case Verdict::fail_certified:
        return "FAIL_CERTIFIED";
    case Verdict::unknown:
        return "UNKNOWN";
    }
    return "UNKNOWN";
}

Result solve(const Problem& problem, const SearchBudget& budget)
{
    const auto rules = make_rules(problem.rules, budget.cascade_limit);
    const auto options = make_options(budget);
    const auto state = make_state(problem);

    std::vector<m3::FirstSwapStats> first_swaps;
    const auto solved = m3::ResumeSolver(rules, options).solve(
        state, m3::ResumeSchedule::staged, &first_swaps);
    const auto& metrics = solved.stats;
    Result result;
    result.work = {
        .candidate_checks = metrics.candidates,
        .refill_nodes = metrics.refill.nodes,
        .states = metrics.states,
        .legal_swaps = m3::legal_swaps(state.board).size(),
        .safe_swaps = static_cast<std::uint64_t>(std::ranges::count_if(first_swaps, [](const auto& swap) {
            return swap.status == m3::Status::win;
        })),
        .candidate_cutoffs = metrics.candidate_cutoffs,
        .refill_cutoffs = metrics.refill_entry_cutoffs + metrics.refill.node_cutoffs,
        .state_cutoffs = metrics.state_cutoffs,
        .cascade_cutoffs = metrics.cascade_cutoffs,
    };
    if (solved.status == m3::Status::fail) {
        result.verdict = Verdict::fail_certified;
        return result;
    }
    if (solved.status == m3::Status::unknown) {
        return result;
    }
    if (solved.certificate.nodes.size() > max_certificate_nodes) {
        result.work.certificate_limit = true;
        return result;
    }
    if (!m3::verify_certificate(state, rules, solved.certificate)) {
        throw std::logic_error("solver produced an invalid winning certificate");
    }

    CertificateBuffer buffer;
    std::ostream output(&buffer);
    output.imbue(std::locale::classic());
    try {
        m3::write_certificate(output, rules, solved.certificate);
    } catch (const std::runtime_error&) {
        if (!buffer.limit_reached()) {
            throw;
        }
        result.work.certificate_limit = true;
        return result;
    }
    result.certificate = buffer.take();
    result.verdict = Verdict::win_certified;
    return result;
}

bool verify_certificate(const Problem& expected, std::string_view certificate, int cascade_limit) noexcept
{
    if (certificate.empty() || certificate.size() > max_certificate_bytes) {
        return false;
    }
    try {
        const auto rules = make_rules(expected.rules, cascade_limit);
        const auto state = make_state(expected);
        const auto cert = parse_certificate(certificate, state, rules);
        return m3::verify_certificate(state, rules, cert);
    } catch (...) {
        return false;
    }
}

} // namespace match3_strict
